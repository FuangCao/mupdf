#include <fitz.h>
#include <mupdf.h>
#include <jpeglib.h>

#define NELEM(a) \
	(sizeof(a) / sizeof((a)[0]))

#define pr_error_info(fmt, args ...) \
	fprintf(stderr, fmt "\n", ##args)

static void jpeg_output_message(j_common_ptr cinfo)
{
	char buff[JMSG_LENGTH_MAX];

	(*cinfo->err->format_message) (cinfo, buff);

	fprintf(stderr, "%s\n", buff);
}

static void init_jpeg_compress_struct(struct jpeg_compress_struct *info, struct jpeg_error_mgr *err, FILE *fout, int width, int height)
{
	info->err = jpeg_std_error(err);
	err->output_message = jpeg_output_message;

	jpeg_create_compress(info);
	info->in_color_space = JCS_RGB;
	jpeg_set_defaults(info);
	jpeg_stdio_dest(info, fout);

	info->input_components = 3;
	info->image_width = width;
	info->image_height = height;
	info->density_unit = 1;
	info->X_density = 150;
	info->Y_density = 150;

	// jpeg_set_quality(info, 100, 1);
	// jpeg_simple_progression(info);

	jpeg_start_compress(info, 1);
}

static unsigned char *RGB8888_RGB888(unsigned int *from, unsigned char *to, int count)
{
	unsigned int *from_end;

	for (from_end = from + count; from < from_end; from++, to += 3)
	{
		const char *temp = (const char *)from;
		to[0] = temp[0];
		to[1] = temp[1];
		to[2] = temp[2];
	}

	return to;
}

static fz_error fz_write_jpeg_fp(fz_pixmap *pixmap, FILE *fp)
{
	struct jpeg_error_mgr jerr;
	struct jpeg_compress_struct cinfo;
	unsigned char *p, *p_end;
	unsigned char buff[pixmap->w * 3], *p_buff = buff;
	int line_size;

	init_jpeg_compress_struct(&cinfo, &jerr, fp, pixmap->w, pixmap->h);

	line_size = pixmap->w * pixmap->n;

	for (p = pixmap->samples, p_end = p + pixmap->h * line_size; p < p_end; p += line_size)
	{
		RGB8888_RGB888((unsigned int *)p, buff, pixmap->w);
		jpeg_write_scanlines(&cinfo, (JSAMPARRAY)&p_buff, 1);
	}

	jpeg_finish_compress(&cinfo);

	return fz_okay;
}

static fz_error fz_write_jpeg(fz_pixmap *pixmap, const char *filename)
{
	fz_error ret;
	FILE *fp;

	if (filename)
	{
		fp = fopen(filename, "wb");
		if (fp == NULL)
		{
			return fz_throw("open file '%s' failed!", filename);
		}
	}
	else
	{
		fp = stdout;
	}

	ret = fz_write_jpeg_fp(pixmap, fp);
	fclose(fp);

	return ret;
}

static void die(fz_error error)
{
	fz_catch(error, "aborting");
	exit(1);
}

static void drawpage(pdf_xref *xref, const char *jpgfile, int pagenum)
{
	fz_error error;
	pdf_page *page;
	fz_device *dev;
	float zoom;
	fz_matrix ctm;
	fz_bbox bbox;
	fz_pixmap *pix;
	fz_glyph_cache *glyphcache;

	glyphcache = fz_new_glyph_cache();

	error = pdf_load_page(&page, xref, pagenum);
	if (error)
		die(fz_rethrow(error, "cannot load page %d failed", pagenum));

	zoom = 5;
	ctm = fz_translate(0, -page->mediabox.y1);
	ctm = fz_concat(ctm, fz_scale(zoom, -zoom));
	ctm = fz_concat(ctm, fz_rotate(page->rotate));
	ctm = fz_concat(ctm, fz_rotate(0));
	bbox = fz_round_rect(fz_transform_rect(ctm, page->mediabox));

	pix = fz_new_pixmap_with_rect(fz_device_rgb, bbox);
	fz_clear_pixmap_with_color(pix, 255);

	dev = fz_new_draw_device(glyphcache, pix);
	pdf_run_page(xref, page, dev, ctm);
	fz_free_device(dev);
	fz_write_jpeg(pix, jpgfile);

	fz_drop_pixmap(pix);
	pdf_free_page(page);
	pdf_age_store(xref->store, 3);
	fz_free_glyph_cache(glyphcache);
	fz_flush_warnings();
}

static int char2value(char c)
{
	switch (c)
	{
	case '0' ... '9':
		return c - '0';

	case 'a' ... 'z':
		return c - 'a' + 10;

	case 'A' ... 'Z':
		return c - 'A' + 10;

	default:
		pr_error_info("invalid char '%c'", c);
	}

	return -1;
}

static unsigned int text2value_unsigned(const char *text, int base)
{
	unsigned int value;

	if (*text == 0)
	{
		switch (*++text)
		{
		case 0:
			return 0;

		case 'x':
		case 'X':
			base = 16;
			break;

		case 'b':
		case 'B':
			base = 2;
			break;

		case '0' ... '7':
			base = 8;
			break;

		default:
			base = 10;
		}
	}

	if (base  < 2)
	{
		base = 10;
	}

	for (value = 0; *text; text++)
	{
		int temp = char2value(*text);
		if (temp < 0)
		{
			return 0;
		}

		value = value * base + temp;
	}

	return value;
}

static int text2value(const char *text, int base)
{
	if (*text == '-')
	{
		return -text2value_unsigned(text + 1, base);
	}
	else
	{
		return text2value_unsigned(text, base);
	}
}

static pdf_xref *cavan_open_pdf(const char *filename)
{
	fz_error error;
	pdf_xref *xref;

	error = pdf_open_xref(&xref, filename, NULL);
	if (error)
	{
		die(fz_rethrow(error, "cannot open document: %s", filename));
		return NULL;
	}

	error = pdf_load_page_tree(xref);
	if (error)
	{
		die(fz_rethrow(error, "cannot load page tree: %s", filename));
		return NULL;
	}

	return xref;
}

static int get_pdf_page_count(int argc, char *argv[])
{
	assert(argc > 0);

	pdf_xref *xref = cavan_open_pdf(argv[0]);
	if (xref == NULL)
	{
		return -1;
	}

	printf("%d", xref->page_len);

	pdf_free_xref(xref);

	return 0;
}

static int pdf2jpeg(int argc, char *argv[])
{
	int pagenum;

	assert(argc > 1);

	pdf_xref *xref = cavan_open_pdf(argv[0]);
	if (xref == NULL)
	{
		return -1;
	}

	pagenum = text2value(argv[1], 10);

	if (argc > 2)
	{
		drawpage(xref, argv[2], pagenum);
	}
	else
	{
		drawpage(xref, NULL, pagenum);
	}

	pdf_free_xref(xref);

	return 0;
}

static char *find_byte(char *text, char *text_end, char c)
{
	while (text < text_end)
	{
		if (*text == c)
		{
			return text;
		}

		text++;
	}

	return NULL;
}

static int read_args_from_stdin(char *argv[], int count)
{
	static char buff[1024], *p, *p_end;
	ssize_t readlen;

	readlen = fread(buff, 1, sizeof(buff), stdin);
	if (readlen < 0)
	{
		return 0;
	}

	buff[readlen] = 0;

	for (p = buff, p_end = p + readlen, count = 0; p < p_end; p++, count++)
	{
		argv[count] = p;
		p = find_byte(p, p_end, '\n');
		if (p == NULL)
		{
			break;
		}

		*p = 0;
	}

	return count;
}

int main(int argc, char *argv[])
{
	const char *subcmd;
	char *my_argv[20];

	assert(argc > 1);

	subcmd = argv[1];
	argc = read_args_from_stdin(my_argv, NELEM(my_argv));
	if (argc < 0)
	{
		return argc;
	}

	if (strcmp(subcmd, "page_count") == 0)
	{
		return get_pdf_page_count(argc, my_argv);
	}
	else if (strcmp(subcmd, "pdf2jpeg") == 0)
	{
		return pdf2jpeg(argc, my_argv);
	}
	else
	{
		pr_error_info("invalid subcmd: %s", subcmd);
		return -1;
	}
}
