# LZO GPU Plugin for CRIU

This plugin provides GPU-accelerated LZO compression and decompression for CRIU memory dumps.

## Features

- **GPU Acceleration**: Uses OpenCL to accelerate LZO compression/decompression on GPU
- **Transparent Integration**: Seamlessly integrates with CRIU's memory dump/restore process
- **Automatic Fallback**: Falls back to CPU compression if GPU is unavailable
- **Configurable**: Can be enabled/disabled via CRIU configuration options

## Requirements

- OpenCL runtime (ocl-icd + vendor-specific drivers)
- OpenCL headers (opencl-headers package)
- CRIU with plugin support

## Installation

1. Ensure OpenCL is installed on your system:
   ```bash
   # Ubuntu/Debian
   sudo apt-get install opencl-headers ocl-icd-opencl-dev

   # CentOS/RHEL
   sudo yum install opencl-headers ocl-icd-devel
   ```

2. Build and install the plugin:
   ```bash
   cd criu/plugins/lzo_gpu
   make
   sudo make install
   ```

## Usage

1. Enable compression in CRIU:
   ```bash
   criu dump --compress -t <PID>
   ```

2. The plugin will automatically:
   - Detect available GPU devices
   - Load and compile OpenCL kernels
   - Use GPU for compression during dump
   - Use GPU for decompression during restore

## Architecture

### Files

- `lzo_gpu_plugin.c` - Main plugin implementation
- `decompress.cl` - OpenCL kernel for decompression
- `decompress_safe.cl` - Safe decompression kernel
- `lzo.cl` - OpenCL kernel for compression
- `minilzo.h` - LZO header definitions for GPU
- `Makefile` - Build configuration

### Workflow

1. **Dump Phase**:
   - Plugin initializes GPU resources
   - Memory pages are compressed using GPU
   - Compressed data is stored with compression flag

2. **Restore Phase**:
   - Plugin detects compressed pages by flag
   - GPU decompresses data on demand
   - Decompressed pages are restored to memory

## Configuration

The plugin respects CRIU's `--compress` option:
- When enabled: Uses GPU for compression/decompression
- When disabled: Plugin is bypassed, uses standard CRIU compression

## Troubleshooting

### Plugin Not Loading
- Check OpenCL installation: `clinfo`
- Verify plugin is in CRIU plugin directory
- Check CRIU logs for plugin initialization errors

### GPU Not Detected
- Ensure GPU drivers are installed
- Check OpenCL runtime: `ls /etc/OpenCL/vendors/`
- Try running with `--compress` option

### Performance Issues
- Plugin automatically falls back to CPU if GPU compression is slower
- Consider GPU memory bandwidth vs. CPU compression ratio

## Development

To modify the plugin:

1. Edit kernel files (`.cl`) for GPU algorithms
2. Modify `lzo_gpu_plugin.c` for CPU-side logic
3. Rebuild with `make && sudo make install`
4. Test with CRIU dump/restore operations

## Limitations

- Requires OpenCL-compatible GPU
- GPU memory limits may affect large dumps
- Current implementation uses basic LZO algorithm
- Plugin is experimental and may have performance overhead for small dumps

## Future Enhancements

- Support for multiple GPU devices
- Async compression/decompression
- Advanced compression algorithms
- Memory pool optimization
- Performance profiling and tuning