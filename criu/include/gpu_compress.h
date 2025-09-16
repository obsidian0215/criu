#ifndef __CR_GPU_COMPRESS_H__
#define __CR_GPU_COMPRESS_H__

/*
 * GPU Compression Interface
 *
 * This header provides the interface for GPU-accelerated compression
 * in CRIU. When GPU compression is not available, these functions
 * provide fallback behavior.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize GPU compression module
 * @return 0 on success, -1 on failure
 */
int gpu_compress_init(void);

/**
 * Cleanup GPU compression module
 */
void gpu_compress_cleanup(void);

/**
 * Check if GPU compression is available
 * @return 1 if available, 0 otherwise
 */
int gpu_compress_available(void);

/**
 * Compress data using GPU acceleration
 * @param input_data Input data buffer
 * @param input_size Size of input data
 * @param output_data Output buffer for compressed data
 * @param output_size Pointer to store compressed data size
 * @return 0 on success, -1 on failure
 */
int gpu_compress_data(const void *input_data, size_t input_size,
                     void *output_data, size_t *output_size);

/**
 * Decompress data using GPU acceleration
 * @param input_data Input compressed data buffer
 * @param input_size Size of compressed data
 * @param output_data Output buffer for decompressed data
 * @param output_size Pointer to store decompressed data size
 * @return 0 on success, -1 on failure
 */
int gpu_decompress_data(const void *input_data, size_t input_size,
                       void *output_data, size_t *output_size);

#ifdef __cplusplus
}
#endif

#endif /* __CR_GPU_COMPRESS_H__ */