/* <z64.me> yar.c: decode and encode MM yaz archives */
/* imported from z64compress, with some minor tweaks */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>

#define FERR(x) {         \
   fprintf(stderr, x);    \
   fprintf(stderr, "\n"); \
   exit(EXIT_FAILURE);    \
}

/* surely an archive won't exceed 64 MB */
#define YAR_MAX (1024 * 1024 * 64)

/* align out address before writing compressed file */
#define FILE_ALIGN          \
   while ((outSz % align))  \
   {                        \
      out[outSz] = 0;       \
      outSz += 1;           \
   }

struct yarFile
{
	int   idx;       /* original index in list */
	int   ofs;       /* global offset of file */
};

static
unsigned int
u32b(void *src)
{
	unsigned char *arr = src;
	
	return (arr[0] << 24) | (arr[1] << 16) | (arr[2] << 8) | arr[3];
}

static
void
u32wr(void *dst, unsigned int src)
{
	unsigned char *arr = dst;
	
	arr[0] = src >> 24;
	arr[1] = src >> 16;
	arr[2] = src >>  8;
	arr[3] = src;
}

static
void
progress(const char *name, int progress, int end)
{
	fprintf(
		stderr
		, "\r""repacking '%s' %d/%d: "
		, name
		, progress
		, end
	);
}

/* reencode yar archive */
/* returns 0 on success, pointer to error message otherwise */
char *
yar_reencode(
	unsigned char   *src     /* source archive */
	, unsigned int   sz      /* source archive size */
	, unsigned char *dst     /* destination archive */
	, unsigned int  *dst_sz  /* destination archive size  */
	, int align              /* compressed file alignment */
	
	, const char *name       /* name of archive (0 = hide progress) */
	, const char *codec      /* the expected encoding header "Yaz0" */
	, void *imm              /* intermediate buffer for conversion  */
	, void *ctx              /* compression context (if applicable) */
	
	, unsigned int *headerLen /* length of the header */
	
	/* decompresses file; return non-zero on fail; optional
	 * if files are already decompressed (up to user to know)
	 */
	, int decode(void *src, void *dst, unsigned dstSz, unsigned *srcSz)
	
	/* compress file; returns non-zero on fail; optional if
	 * files are desired to be left decompressed
	 */
	, int encode(void *src, unsigned srcSz, void *dst, unsigned *dstSz, void *ctx)
	
	/* test if file has been previously encoded; optional */
	, int exist(void *src, unsigned srcSz, void *dst, unsigned *dstSz)
)
{
	unsigned char *ss;
	unsigned char *out;
	unsigned int end;
	unsigned int end_out;
	unsigned int outSz = 0;
	int progress_end;
	struct yarFile *list = 0;
	struct yarFile *item;
	int list_num;
	int cur_index = 1; /* accounts for padding the end of the file */
	
	assert(src);
	assert(sz);
	assert(dst_sz);
	assert(dst);
	assert(align >= 0);
	assert((align & 3) == 0); /* cannot have alignment smaller than 4 */
	
	out = dst;

	end = 0;
	
	ss = src;
	item = list;
	do
	{
		unsigned ofs;
		unsigned uncompSz;
		unsigned OG_encSz;
		unsigned char *b;
		
		ofs = u32b(ss) + end;
		
		/* first entry points to end of list, and first file */
		if (!end)
		{
			end = ofs;
			outSz = end;
			
			/* allocate file list */
			list_num = (end / 4) + 1;
			list = calloc(list_num, sizeof(*list));
			if (!list)
				return "memory error";
			item = list;
			
			FILE_ALIGN
			
			/* output file may be aligned differently */
			end_out = outSz;
			
			progress_end = end / 4;
		}
		
		/* b now points to compressed file */
		b = src + ofs;
		
		/* update progress display */
		if (name)
			progress(name, (ss - src) / 4, progress_end);
		
		/* there should be room for 4-byte codec and 4-byte size */
		if (b + 4 >= src + sz)
			break;
		
		/* decompressed file size is second word */
		uncompSz = u32b(b + 4);
		
		/* yaz-encoded file */
		if (!memcmp(b, codec, 4))
		{
			unsigned char *fout = out + outSz;
			unsigned encSz;
			
			/* user doesn't want encoded data */
			if (!encode)
			{
				imm = fout;
				encSz = uncompSz;
			}
			
			/* decode 'b' only if user provided decoder */
			if (decode)
			{
				if (decode(b, imm, uncompSz, &OG_encSz))
					return "decoder error";
			}
			/* if no decoder is provided, direct copy */
			else
				memcpy(imm, b + 0x10, uncompSz);
			
			/* encode only if user wants that */
			if (encode)
			{
				/* if no exist function has been provided, or
				 * if it hasn't been encoded yet, encode it
				 */
				if (!exist || !exist(imm, uncompSz, fout, &encSz))
				{
					if (encode(imm, uncompSz, fout, &encSz, ctx))
						return "encoder error";
				}
			}
			
			/* point current entry to new file location */
			if (ss > src)
				u32wr(out + (ss - src), outSz - end_out);
			
			/* first entry follows different rules */
			else
				u32wr(out + (ss - src), end_out);
			
			/* advance out_sz to immediately after current file */
			outSz += encSz;
			
			/* align output */
			FILE_ALIGN
		}
		
		/* end of list */
		else if (u32b(b) == 0)
			break;
		
		/* unknown codec */
		else
		{
			char *errmsg = (char*)out;
			char srep[16];
			sprintf(srep, "%08x", u32b(b));
			sprintf(
				errmsg
				, "unknown codec 0x%s encountered at %08X!\n"
				, srep
				, ofs
			);
			return errmsg;
		}
		
		ss += 4;
		item += 1;
		cur_index += 1;
	
	} while ((unsigned)(ss - src) < end && cur_index < list_num - 1);
	
	/* update progress display */
	if (name)
		progress(name, progress_end, progress_end);
	
	/* point final entry to end (four 00 bytes) */
	u32wr(out + (ss - src), outSz - end_out);
	memset(out + outSz, 0, 16);
	outSz += 4;
	
	*headerLen = end_out;
	
	/* in case list end changed due to padding, make multiple *
	 * end-of-list markers throughout the alignment space     */
	if (end_out > end)
	{
		unsigned i;
		unsigned last = u32b(out + (end - 4));
		for (i = 0; i < (end_out - end) / 4; ++i)
		{
			u32wr(out + end + i * 4, last);
		}
	}
	
	/* align final output size to 16 */
	if (outSz & 15)
		outSz += 16 - (outSz & 15);
	
	/* if new file was constructed, note its size */
	*dst_sz = outSz;
	
	/* cleanup */
	free(list);
	
	/* success */
	return 0;
}

/* 
 * 
 * usage example (writes decompressed archive)
 * 
 */

/* yaz decoder, courtesy of spinout182 */
int spinout_yaz_dec(void *_src, void *_dst, unsigned dstSz, unsigned *srcSz)
{
	unsigned char *src = _src;
	unsigned char *dst = _dst;
	
	int srcPlace = 0, dstPlace = 0; /*current read/write positions*/
	
	unsigned int validBitCount = 0; /*number of valid bits left in "code" byte*/
	unsigned char currCodeByte = 0;
	
	if (dstSz == 0)
		dstSz = u32b(src + 4);
	
	int uncompressedSize = dstSz;
	
	src += 0x10;
	
	while(dstPlace < uncompressedSize)
	{
		/*read new "code" byte if the current one is used up*/
		if(!validBitCount)
		{
			currCodeByte = src[srcPlace];
			++srcPlace;
			validBitCount = 8;
		}
		
		if(currCodeByte & 0x80)
		{
			/*direct copy*/
			dst[dstPlace] = src[srcPlace];
			dstPlace++;
			srcPlace++;
		}
		else
		{
			/*RLE part*/
			unsigned char byte1 = src[srcPlace];
			unsigned char byte2 = src[srcPlace + 1];
			srcPlace += 2;
			
			unsigned int dist = ((byte1 & 0xF) << 8) | byte2;
			unsigned int copySource = dstPlace - (dist + 1);

			unsigned int numBytes = byte1 >> 4;
			if(numBytes)
				numBytes += 2;
			else
			{
				numBytes = src[srcPlace] + 0x12;
				srcPlace++;
			}

			/*copy run*/
			unsigned int i;
			for(i = 0; i < numBytes; ++i)
			{
				dst[dstPlace] = dst[copySource];
				copySource++;
				dstPlace++;
			}
		}
		
		/*use next bit from "code" byte*/
		currCodeByte <<= 1;
		validBitCount-=1;		
	}
	
	return 0;
	
	(void)srcSz;
}


/* encodes decompressed data, storing result in dst */
static
int encode(void *src, unsigned srcSz, void *_dst, unsigned *dstSz, void *ctx)
{
	unsigned char *dst = _dst;
	
#if 0
/* header */
	/* codec */
	memcpy(dst, "raw0", 4);
	
	/* decompressed size */
	u32wr(dst + 4, srcSz);
	
	/* 8 more bytes of padding */
	memset(dst + 8, 0, 8);
#endif
	
/* contents */
	/* direct copy (data left unencoded; you could encode here though) */
	memcpy(dst/* + 0x10*/, src, srcSz);
	*dstSz = srcSz;// + 0x10;
	
	return 0;
	
	(void)ctx;
}

/* checks if data has already been encoded */
/* if it does, dst is filled with that data and 1 is returned */
/* 0 is returned otherwise */
static
int exist(void *src, unsigned srcSz, void *dst, unsigned *dstSz)
{
	return 0;
	
	(void)src;
	(void)srcSz;
	(void)dst;
	(void)dstSz;
}

/* unsafe but it's a test program so it's fine */
static
unsigned char *
file_read(const char *fn, unsigned *sz)
{
	FILE *fp;
	unsigned char *raw;
	
	assert(fn);
	assert(sz);
	
	fp = fopen(fn, "rb");
	if (!fp)
		FERR("failed to open file for reading");
	
	fseek(fp, 0, SEEK_END);
	*sz = ftell(fp);
	
	if (!sz)
		FERR("read file size == 0");
	
	fseek(fp, 0, SEEK_SET);
	raw = malloc(*sz);
	if (!raw)
		FERR("memory error");
	
	if (fread(raw, 1, *sz, fp) != *sz)
		FERR("file read error");
	
	fclose(fp);
	return raw;
}

/* minimal file writer
 * returns 0 on failure
 * returns non-zero on success
 */
int savefile(const char *fn, const void *dat, const size_t sz)
{
	FILE *fp;
	
	/* rudimentary error checking returns 0 on any error */
	if (
		!fn
		|| !sz
		|| !dat
		|| !(fp = fopen(fn, "wb"))
		|| fwrite(dat, 1, sz, fp) != sz
		|| fclose(fp)
	)
		return 0;
	
	return 1;
}

int unyar(const char *infn, const char *outfn, int isHeaderless)
{
	void *raw;
	unsigned int raw_sz;
	
	void *out;
	void *imm;
	unsigned int out_sz = 0;
	unsigned int headerLen = 0;
	
	const char *errmsg;
	
	raw = file_read(infn, &raw_sz);
	fprintf(stderr, "input file %s:\n", infn);
	
	/* surely an archive won't exceed 64 MB */
	out = malloc(1024 * 1024 * 64);
	imm = malloc(1024 * 1024 * 64);
	
	if ((errmsg = yar_reencode(
		raw, raw_sz, out, &out_sz, 16, infn, "Yaz0", imm, 0, &headerLen
		, spinout_yaz_dec
		, encode
		, exist
	)))
	{
		fprintf(stderr, "unyar error: %s\n", errmsg);
		exit(EXIT_FAILURE);
	}
	
	fprintf(stderr, "headerLen = %08x\n", headerLen);
	
	/* write output file */
	if ((!isHeaderless && !savefile(outfn, out, out_sz))
		|| (isHeaderless && !savefile(outfn, ((uint8_t*)out) + headerLen, out_sz - headerLen))
	)
		FERR("failed to write output file");
	
	free(raw);
	free(out);
	free(imm);
	
	return 0;
}

