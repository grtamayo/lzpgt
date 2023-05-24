/*
	Filename:     lzpgt.c
	Description:  PPP style or simply LZP.
	Written by:   Gerald R. Tamayo, (8/22/2022)
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>   /* C99 */
#include <time.h>
#include "gtbitio2.c"

#define WBITS       20
#define WSIZE       (1<<WBITS)
#define WMASK       (WSIZE-1)

/* PPP_BLOCKBITS must be >= 3 (multiple of 8 bytes blocksize) */
#define PPP_BLOCKBITS  15
#define PPP_BLOCKSIZE  (1<<PPP_BLOCKBITS)

enum {
	/* modes */
	COMPRESS,
	DECOMPRESS,
};

typedef struct {
	char alg[8];
	int64_t ppp_nblocks;
	int ppp_lastblocksize;
} file_stamp;

unsigned char win_buf[ WSIZE ];   /* the prediction buffer or "GuessTable". */
unsigned char pattern[ PPP_BLOCKSIZE ];   /* the "look-ahead" buffer. */
unsigned char bbuf[PPP_BLOCKSIZE], cbuf[PPP_BLOCKSIZE];
int64_t ppp_nblocks;
int ppp_lastblocksize;

void copyright( void );
void   compress_LZP( unsigned char w[], unsigned char p[] );
void decompress_LZP( unsigned char w[] );

void usage( void )
{
	fprintf(stderr, "\n Usage: lzpgt c|d infile outfile\n"
		"\n Commands:\n  c = PPP (raw byte output) \n  d = decoding.\n"
	);
	copyright();
	exit(0);
}

int main( int argc, char *argv[] )
{
	float ratio = 0.0;
	int mode = -1;
	file_stamp fstamp;
	char *cmd = NULL;
	
	clock_t start_time = clock();
	
	if ( argc != 4 ) usage();
	init_buffer_sizes( (1<<15) );
	
	cmd = argv[1];
	while ( cmd ){
		switch( *cmd ) {
			case 'c':
				if ( mode == -1 ) mode = COMPRESS; else usage();
				if ( *(cmd+1) != 0 ) usage(); cmd++; break;
			case 'd':
				if ( mode == -1 ) mode = DECOMPRESS; else usage();
				if ( *(cmd+1) != 0 ) usage(); cmd++; break;
			case 0: cmd = NULL; break;
			default : usage();
		}
	}
	
	if ( (gIN=fopen( argv[2], "rb" )) == NULL ) {
		fprintf(stderr, "\nError opening input file.");
		return 0;
	}
	if ( (pOUT=fopen( argv[3], "wb" )) == NULL ) {
		fprintf(stderr, "\nError opening output file.");
		return 0;
	}
	init_put_buffer();
	
	/* initialize prediction buffer to all zero (0) values. */
	memset( win_buf, 0, WSIZE );
	
	if ( mode == COMPRESS ){
		/* Write the FILE STAMP. */
		strcpy( fstamp.alg, "LZPGT" );
		fwrite( &fstamp, sizeof(file_stamp), 1, pOUT );
		nbytes_out = sizeof(file_stamp);
		
		fprintf(stderr, "\n Encoding [ %s to %s ] ...", 
			argv[2], argv[3] );
		compress_LZP( win_buf, pattern );
	}
	else if ( mode == DECOMPRESS ){
		fread( &fstamp, sizeof(file_stamp), 1, gIN );
		ppp_lastblocksize = fstamp.ppp_lastblocksize;
		ppp_nblocks = fstamp.ppp_nblocks;
		init_get_buffer();
		nbytes_read = sizeof(file_stamp);
		
		fprintf(stderr, "\n Decoding...");
		decompress_LZP( win_buf );
		nbytes_read = get_nbytes_read();
		free_get_buffer();
	}
	flush_put_buffer();
	
	if ( mode == COMPRESS ) {
		rewind( pOUT );
		fstamp.ppp_nblocks = ppp_nblocks;
		fstamp.ppp_lastblocksize = ppp_lastblocksize;
		fwrite( &fstamp, sizeof(file_stamp), 1, pOUT );
	}
	
	fprintf(stderr, "done.\n  %s (%lld) -> %s (%lld)", 
		argv[2], nbytes_read, argv[3], nbytes_out);
	if ( mode == COMPRESS ) {
		ratio = (((float) nbytes_read - (float) nbytes_out) /
			(float) nbytes_read ) * (float) 100;
		fprintf(stderr, "\n Compression ratio: %3.2f %%", ratio );
	}
	fprintf(stderr, " in %3.2f secs.\n", (double) (clock()-start_time) / CLOCKS_PER_SEC );
	
	free_put_buffer();
	if ( gIN ) fclose( gIN );
	if ( pOUT ) fclose( pOUT );
	return 0;
}

void copyright( void )
{
	fprintf(stderr, "\n Written by: Gerald R. Tamayo (c) 2022-2023\n");
}

/* PPP style, a simple preprocessor. */
void compress_LZP( unsigned char w[], unsigned char p[] )
{
	int b = 0, c, n, i, nread, prev=0;  /* prev = context hash */
	
	ppp_nblocks = 0;
	ppp_lastblocksize = 0;
	while ( (nread=fread(p, 1, PPP_BLOCKSIZE, gIN)) ){
		n = 0;
		while ( n < nread ) {
			if ( w[prev] == (c=p[n]) ){  /* Guess/prediction correct */
				put_ONE();
			}
			else {
				put_ZERO();
				w[prev] = c;
				cbuf[b++] = c;  /* record mismatched byte */
			}
			prev = ((prev<<5)+c) & WMASK;  /* update hash */
			n++;
		}
		nbytes_read += nread;
		
		/* write mismatched bytes. */
		if ( nread == PPP_BLOCKSIZE ){
			for ( i = 0; i < b; i++  ) {
				pfputc( cbuf[i] );
			}
			b = 0;
			ppp_nblocks++;
		}
		else if ( nread < PPP_BLOCKSIZE ){ /* last blocksize mismatched bytes */
			/* tricky bits in current *pbuf. */
			if ( p_cnt > 0 && p_cnt < 8 ){
				p_cnt = 7;       /* force byte boundary. */
				advance_buf();   /* writes *pbuf */
			}
			/* write mismatched bytes. */
			for ( i = 0; i < b; i++  ) {
				pfputc( cbuf[i] );
			}
			ppp_lastblocksize = nread;
		}
	}
}

void decompress_LZP( unsigned char w[] )
{
    int c = 0, i = 0, prev = 0;  /* prev = context hash */
	
	if ( ppp_nblocks > 0 ) while ( ppp_nblocks-- ) {
		/* get the block of bits */
		for ( i = 0; i < PPP_BLOCKSIZE; i++ ){
			if ( get_bit() ) bbuf[i] = 1;
			else bbuf[i] = 0;
		}
		
		/* Output bytes. */
		for ( i = 0; i < PPP_BLOCKSIZE; i++ ){
			if ( bbuf[i] == 1 ){
				pfputc( c=w[prev] );
			}
			else {
				pfputc( c=gfgetc() );
				w[prev] = c;
			}
			prev = ((prev<<5)+c) & WMASK;
		}
	}
	
	/* last block */
	if ( ppp_lastblocksize > 0 ) {
		/* get the block of bits */
		for ( i = 0; i < ppp_lastblocksize; i++ ){
			if ( get_bit() ) bbuf[i] = 1;
			else bbuf[i] = 0;
		}
		if ( g_cnt > 0 && g_cnt < 8 ) {
			/* tricky bits. advance gbuf. */
			g_cnt = 7;
			advance_gbuf();
		}
		
		/* Output bytes. */
		for ( i = 0; i < ppp_lastblocksize; i++ ){
			if ( bbuf[i] == 1 ){
				pfputc( c=w[prev] );
			}
			else {
				pfputc( c=gfgetc() );
				w[prev] = c;
			}
			prev = ((prev<<5)+c) & WMASK;
		}
	}
}
