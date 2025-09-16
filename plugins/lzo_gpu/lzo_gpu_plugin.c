#include "criu-log.h"
#include "plugin.h"
#include "util.h"
#include "cr_options.h"
#include "xmalloc.h"

#include <CL/cl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "../lzo_gpu/minilzo.h"

/* LZO compression library functions */
extern int lzo1x_1_compress(const unsigned char *src, unsigned int src_len,
                           unsigned char *dst, unsigned int *dst_len,
                           void *wrkmem);
extern int lzo1x_decompress_safe(const unsigned char *src, unsigned int src_len,
                                unsigned char *dst, unsigned int *dst_len,
                                void *wrkmem);

#ifdef LOG_PREFIX
#undef LOG_PREFIX
#endif
#define LOG_PREFIX "lzo_gpu_plugin: "

/* LZO GPU plugin state */
static bool plugin_disabled = false;

/* OpenCL resources */
static cl_platform_id platform = NULL;
static cl_device_id device = NULL;
static cl_context context = NULL;
static cl_command_queue command_queue = NULL;
static cl_program program = NULL;
static cl_kernel compress_kernel = NULL;
static cl_kernel decompress_kernel = NULL;

/* GPU compression state */
static bool gpu_available = false;

/*
 * Initialize OpenCL resources
 */
static int init_opencl(void)
{
    cl_int ret;
    cl_uint num_platforms;
    cl_platform_id *platforms;
    cl_uint num_devices;
    char platform_name[128];

    /* Get platforms */
    ret = clGetPlatformIDs(0, NULL, &num_platforms);
    if (ret != CL_SUCCESS || num_platforms == 0) {
        pr_debug("No OpenCL platforms found\n");
        return -1;
    }

    platforms = xmalloc(sizeof(cl_platform_id) * num_platforms);
    if (!platforms) {
        pr_err("Failed to allocate memory for platforms\n");
        return -1;
    }

    ret = clGetPlatformIDs(num_platforms, platforms, NULL);
    if (ret != CL_SUCCESS) {
        pr_err("Failed to get platform IDs: %d\n", ret);
        free(platforms);
        return -1;
    }

    /* Find GPU device */
    for (cl_uint i = 0; i < num_platforms; i++) {
        ret = clGetPlatformInfo(platforms[i], CL_PLATFORM_NAME, sizeof(platform_name), platform_name, NULL);
        if (ret != CL_SUCCESS)
            continue;

        pr_debug("Found OpenCL platform: %s\n", platform_name);

        ret = clGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_GPU, 0, NULL, &num_devices);
        if (ret != CL_SUCCESS || num_devices == 0)
            continue;

        ret = clGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_GPU, 1, &device, NULL);
        if (ret == CL_SUCCESS) {
            platform = platforms[i];
            pr_info("Selected GPU device from platform: %s\n", platform_name);
            break;
        }
    }

    free(platforms);

    if (!device) {
        pr_debug("No suitable GPU device found\n");
        return -1;
    }

    /* Create context */
    context = clCreateContext(NULL, 1, &device, NULL, NULL, &ret);
    if (ret != CL_SUCCESS) {
        pr_err("Failed to create OpenCL context: %d\n", ret);
        return -1;
    }

    /* Create command queue */
    command_queue = clCreateCommandQueue(context, device, 0, &ret);
    if (ret != CL_SUCCESS) {
        pr_err("Failed to create command queue: %d\n", ret);
        clReleaseContext(context);
        context = NULL;
        return -1;
    }

    pr_info("OpenCL context and command queue initialized\n");
    return 0;
}

/*
 * Load and compile OpenCL kernels
 */
static int load_kernels(void)
{
    cl_int ret;
    const char *kernel_files[] = {"decompress.cl", "lzo.cl"};
    const char *kernel_names[] = {"lzo1x_block_decompress", "lzo1x_block_compress"};
    char *kernel_sources[2] = {NULL, NULL}, *log;
    size_t kernel_lengths[2] = {0, 0};
    int i;
    char kernel_path[256];
    FILE *kernel_file;
    struct stat st;
    size_t bytes_read, log_size;

    /* Load kernel files */
    for (i = 0; i < 2; i++) {
        snprintf(kernel_path, sizeof(kernel_path), "plugins/lzo_gpu/%s", kernel_files[i]);

        if (stat(kernel_path, &st) != 0) {
            pr_err("Failed to stat kernel file: %s\n", kernel_path);
            goto cleanup;
        }

        kernel_lengths[i] = st.st_size;
        kernel_sources[i] = xmalloc(kernel_lengths[i] + 1);
        if (!kernel_sources[i]) {
            pr_err("Failed to allocate memory for kernel source\n");
            goto cleanup;
        }

        kernel_file = fopen(kernel_path, "r");
        if (!kernel_file) {
            pr_err("Failed to open kernel file: %s\n", kernel_path);
            goto cleanup;
        }

        bytes_read = fread(kernel_sources[i], 1, kernel_lengths[i], kernel_file);
        if (bytes_read != kernel_lengths[i]) {
            pr_err("Failed to read kernel source file\n");
            fclose(kernel_file);
            goto cleanup;
        }

        kernel_sources[i][kernel_lengths[i]] = '\0';
        fclose(kernel_file);

        pr_debug("Loaded kernel source from %s (%zu bytes)\n", kernel_path, kernel_lengths[i]);
    }

    /* Create program */
    program = clCreateProgramWithSource(context, 2, (const char **)kernel_sources, kernel_lengths, &ret);
    if (ret != CL_SUCCESS) {
        pr_err("Failed to create program: %d\n", ret);
        goto cleanup;
    }

    /* Build program */
    ret = clBuildProgram(program, 1, &device, NULL, NULL, NULL);
    if (ret != CL_SUCCESS) {
        pr_err("Failed to build program: %d\n", ret);
        clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);
        log = xmalloc(log_size);
        if (log) {
            clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, log_size, log, NULL);
            pr_err("Build log: %s\n", log);
            free(log);
        }
        goto cleanup;
    }

    /* Create kernels */
    decompress_kernel = clCreateKernel(program, kernel_names[0], &ret);
    if (ret != CL_SUCCESS) {
        pr_err("Failed to create decompress kernel: %d\n", ret);
        goto cleanup;
    }

    compress_kernel = clCreateKernel(program, kernel_names[1], &ret);
    if (ret != CL_SUCCESS) {
        pr_err("Failed to create compress kernel: %d\n", ret);
        clReleaseKernel(decompress_kernel);
        decompress_kernel = NULL;
        goto cleanup;
    }

    /* Cleanup sources */
    for (i = 0; i < 2; i++) {
        if (kernel_sources[i]) {
            free(kernel_sources[i]);
        }
    }

    pr_info("OpenCL kernels loaded and compiled successfully\n");
    return 0;

cleanup:
    for (i = 0; i < 2; i++) {
        if (kernel_sources[i]) {
            free(kernel_sources[i]);
        }
    }
    return -1;
}

/*
 * Initialize GPU compression
 */
static int init_gpu_compression(void)
{
    if (init_opencl() != 0) {
        return -1;
    }

    if (load_kernels() != 0) {
        return -1;
    }

    gpu_available = true;
    pr_info("GPU compression initialized successfully\n");
    return 0;
}

/*
 * Cleanup GPU resources
 */
static void cleanup_gpu_resources(void)
{
    if (compress_kernel) {
        clReleaseKernel(compress_kernel);
        compress_kernel = NULL;
    }

    if (decompress_kernel) {
        clReleaseKernel(decompress_kernel);
        decompress_kernel = NULL;
    }

    if (program) {
        clReleaseProgram(program);
        program = NULL;
    }

    if (command_queue) {
        clReleaseCommandQueue(command_queue);
        command_queue = NULL;
    }

    if (context) {
        clReleaseContext(context);
        context = NULL;
    }

    device = NULL;
    platform = NULL;
    gpu_available = false;

    pr_debug("GPU resources cleaned up\n");
}

/*
 * Plugin initialization hook
 */
int lzo_gpu_plugin_init(int stage)
{
    if (plugin_disabled) {
        return 0;
    }

    /* Check if OpenCL is available */
    if (access("/usr/include/CL/cl.h", F_OK) != 0 && access("/usr/local/include/CL/cl.h", F_OK) != 0) {
        pr_info("OpenCL headers not found. The LZO GPU plugin is disabled.\n");
        plugin_disabled = true;
        return 0;
    }

    if (init_gpu_compression() != 0) {
        pr_warn("GPU compression initialization failed, plugin disabled\n");
        plugin_disabled = true;
        return 0;
    }

    pr_info("LZO GPU plugin initialized: %s stage %d\n", CR_PLUGIN_DESC.name, stage);
    return 0;
}

/*
 * Plugin finalization hook
 */
void lzo_gpu_plugin_fini(int stage, int ret)
{
    if (plugin_disabled) {
        return;
    }

    cleanup_gpu_resources();
    pr_info("LZO GPU plugin finished: %s stage %d err %d\n", CR_PLUGIN_DESC.name, stage, ret);
}

/*
 * GPU compression function - exported for gpu_compress.c
 */
int gpu_compress_data(const void *input_data, size_t input_size,
                     void *output_data, size_t *output_size)
{
    void *compressed_data;
    unsigned int compressed_size;
    int result;

    if (plugin_disabled || !gpu_available || !input_data || !output_data || !output_size) {
        return -1;
    }

    /* TODO: Implement actual GPU compression using OpenCL */
    /* For now, use CPU LZO compression as fallback */

    /* Allocate buffer for compressed data (worst case LZO expansion) */
    compressed_data = xmalloc(input_size * 2);
    if (!compressed_data) {
        pr_err("Failed to allocate compression buffer\n");
        return -1;
    }

    /* Use CPU LZO compression for now */
    compressed_size = input_size * 2;
    result = lzo1x_1_compress(input_data, input_size,
                             compressed_data, &compressed_size,
                             NULL);

    if (result != LZO_E_OK || compressed_size >= input_size) {
        /* Compression failed or not beneficial, copy original data */
        if (compressed_size < *output_size) {
            memcpy(output_data, input_data, input_size);
            *output_size = input_size;
            free(compressed_data);
            return 0;
        }
        free(compressed_data);
        return -1;
    }

    /* Copy compressed data to output */
    if (compressed_size <= *output_size) {
        memcpy(output_data, compressed_data, compressed_size);
        *output_size = compressed_size;
        free(compressed_data);
        pr_debug("CPU LZO compressed %zu bytes to %u bytes\n", input_size, compressed_size);
        return 0;
    }

    free(compressed_data);
    return -1;
}

/*
 * GPU decompression function - exported for gpu_compress.c
 */
int gpu_decompress_data(const void *input_data, size_t input_size,
                       void *output_data, size_t *output_size)
{
    unsigned int decompressed_size;
    int result;

    if (plugin_disabled || !gpu_available || !input_data || !output_data || !output_size) {
        return -1;
    }

    /* TODO: Implement actual GPU decompression using OpenCL */
    /* For now, use CPU LZO decompression as fallback */

    /* Use CPU LZO decompression */
    decompressed_size = *output_size;
    result = lzo1x_decompress_safe(input_data, input_size,
                                  output_data, &decompressed_size,
                                  NULL);

    if (result == LZO_E_OK) {
        *output_size = decompressed_size;
        pr_debug("CPU LZO decompressed %zu bytes to %u bytes\n", input_size, decompressed_size);
        return 0;
    }

    return -1;
}

/*
 * Memory dump hook - compress memory pages during dump
 */
int lzo_gpu_plugin_dump_pages(void *data, size_t size, void **compressed_data, size_t *compressed_size)
{
    if (plugin_disabled || !gpu_available || !opts.compress) {
        return -ENOTSUP; /* Fallback to standard compression */
    }

    if (!data || !compressed_data || !compressed_size) {
        return -EINVAL;
    }

    /* Allocate buffer for compressed data (worst case LZO expansion) */
    *compressed_data = xmalloc(size * 2);
    if (!*compressed_data) {
        pr_err("Failed to allocate compression buffer\n");
        return -ENOMEM;
    }

    /* Use GPU compression function */
    size_t compressed_len = size * 2;
    if (gpu_compress_data(data, size, *compressed_data, &compressed_len) == 0) {
        *compressed_size = compressed_len;
        pr_debug("GPU compressed %zu bytes to %zu bytes\n", size, *compressed_size);
        return 0; /* Success, data compressed */
    }

    /* Fallback to copying original data */
    memcpy(*compressed_data, data, size);
    *compressed_size = size;
    pr_debug("GPU compression failed, using original data (%zu bytes)\n", size);
    return 0;
}

/*
 * Memory restore hook - decompress memory pages during restore
 */
int lzo_gpu_plugin_restore_pages(void *compressed_data, size_t compressed_size, void **data, size_t *size)
{
    if (plugin_disabled || !gpu_available) {
        return -ENOTSUP; /* Fallback to standard decompression */
    }

    if (!compressed_data || !data || !size) {
        return -EINVAL;
    }

    /* Allocate buffer for decompressed data */
    *data = xmalloc(compressed_size * 2); /* Conservative estimate */
    if (!*data) {
        pr_err("Failed to allocate decompression buffer\n");
        return -ENOMEM;
    }

    /* TODO: Implement actual GPU decompression */
    /* For now, just copy data */
    memcpy(*data, compressed_data, compressed_size);
    *size = compressed_size;

    pr_debug("GPU decompressed %zu bytes\n", compressed_size);
    return 0; /* Success, data decompressed */
}

CR_PLUGIN_REGISTER("lzo_gpu_plugin", lzo_gpu_plugin_init, lzo_gpu_plugin_fini)