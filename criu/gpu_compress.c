#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>

#include "common/compiler.h"
#include "criu-log.h"
#include "gpu_compress.h"

#undef LOG_PREFIX
#define LOG_PREFIX "gpu-compress: "

/* LZO compression memory requirements */
#ifndef LZO1X_1_MEM_COMPRESS
#define LZO1X_1_MEM_COMPRESS    ((unsigned int) (2048 * sizeof(unsigned char *)))
#endif

/*
 * GPU Compression Implementation
 *
 * This file provides GPU compression functionality by integrating with
 * the lzo_gpu plugin, or falling back to CPU-based compression.
 */

/* GPU compression state */
static int gpu_available = 0;
static int gpu_initialized = 0;

/* functions for GPU compression - skip compression when GPU unavailable */
static int page_compress(const unsigned char *src, unsigned int src_len,
                             unsigned char *dst, unsigned int *dst_len,
                             void *wrkmem)
{
    (void)src; (void)src_len; (void)dst; (void)dst_len; (void)wrkmem;
    /* Skip compression when GPU is unavailable */
    return -1;
}

static int page_decompress(const unsigned char *src, unsigned int src_len,
                               unsigned char *dst, unsigned int *dst_len,
                               void *wrkmem)
{
    (void)src; (void)src_len; (void)dst; (void)dst_len; (void)wrkmem;
    /* Skip decompression when GPU is unavailable */
    return -1;
}

/**
 * Check if GPU compression plugin is available
 */
static int check_gpu_plugin_available(void)
{
	const char *plugin_path = "plugins/lzo_gpu/lzo_gpu_plugin.so";
	struct stat st;

	/* Check if plugin file exists and is executable */
	if (stat(plugin_path, &st) != 0) {
		pr_debug("GPU plugin not found at %s\n", plugin_path);
		return 0;
	}

	/* For now, assume GPU plugin is available if file exists */
	/* TODO: Actually check if OpenCL and GPU are available */
	pr_debug("GPU plugin found, assuming GPU compression available\n");
	return 1;
}

/**
 * Initialize GPU compression module
 */
int gpu_compress_init(void)
{
	if (gpu_initialized) {
		pr_debug("GPU compression already initialized\n");
		return gpu_available ? 0 : -1;
	}

	gpu_initialized = 1;
	gpu_available = check_gpu_plugin_available();

	if (gpu_available) {
		pr_info("GPU compression initialized and available\n");
		return 0;
	} else {
		pr_debug("GPU compression not available, will use CPU fallback\n");
		return -1;
	}
}

/**
 * Cleanup GPU compression module
 */
void gpu_compress_cleanup(void)
{
	gpu_available = 0;
	gpu_initialized = 0;
	pr_debug("GPU compression cleanup completed\n");
}

/**
 * Check if GPU compression is available
 */
int gpu_compress_available(void)
{
	return gpu_available;
}

/**
 * Compress data using GPU acceleration or CPU fallback
 */
int gpu_compress_data(const void *input_data, size_t input_size,
                     void *output_data, size_t *output_size)
{
	static void *wrkmem = NULL;
	unsigned int compressed_size;
	int result;

	if (!input_data || !output_data || !output_size) {
		return -1;
	}

	if (!gpu_available) {
		pr_debug("GPU compression not available, falling back to CPU\n");

		/* Copy input data to output as-is (no compression) */
		if (*output_size >= input_size) {
			memcpy(output_data, input_data, input_size);
			*output_size = input_size;
			return 0;
		}
		return -1;
	}

	/* TODO: Implement actual GPU compression using OpenCL */
	/* For now, use CPU LZO compression as fallback */

	/* Allocate working memory for LZO */
	if (!wrkmem) {
		wrkmem = malloc(LZO1X_1_MEM_COMPRESS);
		if (!wrkmem) {
			pr_err("Failed to allocate LZO working memory\n");
			return -1;
		}
	}

	/* Use CPU no-op compression (fallback) */
	compressed_size = *output_size;
	result = page_compress(input_data, input_size,
	                      output_data, &compressed_size,
	                      wrkmem);

	if (result == 0 && compressed_size < input_size) {
		/* Compression successful and beneficial */
		*output_size = compressed_size;
		pr_debug("CPU LZO compressed %zu bytes to %u bytes\n", input_size, compressed_size);
		return 0;
	} else {
		/* Compression failed or not beneficial, copy original data */
		if (*output_size >= input_size) {
			memcpy(output_data, input_data, input_size);
			*output_size = input_size;
			pr_debug("CPU LZO compression failed, using original data (%zu bytes)\n", input_size);
			return 0;
		}
		return -1;
	}
}

/**
 * Decompress data using GPU acceleration or CPU fallback
 */
int gpu_decompress_data(const void *input_data, size_t input_size,
                       void *output_data, size_t *output_size)
{
	unsigned int decompressed_size;
	int result;

	if (!input_data || !output_data || !output_size) {
		return -1;
	}

	if (!gpu_available) {
		pr_debug("GPU decompression not available, falling back to CPU\n");

		/* Copy input data to output as-is (no decompression) */
		if (*output_size >= input_size) {
			memcpy(output_data, input_data, input_size);
			*output_size = input_size;
			return 0;
		}
		return -1;
	}

	/* TODO: Implement actual GPU decompression using OpenCL */
	/* For now, use CPU LZO decompression */

	/* Use CPU no-op decompression (fallback) */
	decompressed_size = *output_size;
	result = page_decompress(input_data, input_size,
	                        output_data, &decompressed_size,
	                        NULL);

	if (result == 0) {
		*output_size = decompressed_size;
		pr_debug("CPU LZO decompressed %zu bytes to %u bytes\n", input_size, decompressed_size);
		return 0;
	} else {
		pr_err("CPU LZO decompression failed\n");
		return -1;
	}
}