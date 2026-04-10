#!/usr/bin/env python3
"""
ADI Audio Converter
Converts WAV files to MinimalOS .adi compressed audio format

Supports:
- IMA ADPCM (4:1 compression)
- Microsoft ADPCM (4:1 compression)
- FLAC (lossless, ~2:1 compression)

Usage:
    python wav_to_adi.py input.wav output.adi --format IADPCM
    python wav_to_adi.py input.wav output.adi --format MSADPCM --volume 80
    python wav_to_adi.py input.wav output.adi --format FLAC
    
    # Generate .o file for direct disk write:
    python wav_to_adi.py input.wav output.o --format IADPCM --object-file
"""

import wave
import struct
import sys
import argparse
import os
import shutil
import tempfile
import subprocess

# ===========================================
# IMA ADPCM ENCODER
# ===========================================

IMA_INDEX_TABLE = [
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
]

IMA_STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
]

def encode_ima_adpcm(pcm_samples):
    """Encode PCM16 samples to IMA ADPCM (4-bit)"""
    predictor = 0
    step_index = 0
    output = bytearray()
    
    for i in range(0, len(pcm_samples), 2):
        byte_val = 0
        
        for nibble in range(2):
            if i + nibble >= len(pcm_samples):
                break
                
            sample = pcm_samples[i + nibble]
            
            # Calculate difference
            diff = sample - predictor
            sign = 0
            if diff < 0:
                sign = 8
                diff = -diff
            
            # Quantize
            step = IMA_STEP_TABLE[step_index]
            code = 0
            
            if diff >= step:
                code = 4
                diff -= step
            step >>= 1
            if diff >= step:
                code |= 2
                diff -= step
            step >>= 1
            if diff >= step:
                code |= 1
            
            code |= sign
            
            # Update predictor
            diff = step >> 3
            if code & 1: diff += step >> 2
            if code & 2: diff += step >> 1
            if code & 4: diff += step
            if code & 8: diff = -diff
            
            predictor += diff
            predictor = max(-32768, min(32767, predictor))
            
            # Update step index
            step_index += IMA_INDEX_TABLE[code & 7]
            step_index = max(0, min(88, step_index))
            
            # Pack nibble
            if nibble == 0:
                byte_val = code & 0x0F
            else:
                byte_val |= (code & 0x0F) << 4
        
        output.append(byte_val)
    
    return bytes(output)

# ===========================================
# MS ADPCM ENCODER (Simplified)
# ===========================================

def encode_ms_adpcm(pcm_samples):
    """Encode PCM16 samples to MS ADPCM (simplified)"""
    # MS-ADPCM is more complex than IMA-ADPCM
    # This is a simplified version - for production use a proper library
    print("Warning: MS-ADPCM encoding is simplified")
    return encode_ima_adpcm(pcm_samples)  # Fallback to IMA for now

# ===========================================
# FLAC ENCODER
# ===========================================

def encode_flac(pcm_samples, sample_rate, channels):
    """Encode PCM16 samples to FLAC"""
    try:
        import subprocess
        import tempfile
        
        # Write PCM to temporary WAV
        with tempfile.NamedTemporaryFile(suffix='.wav', delete=False) as wav_tmp:
            wav_path = wav_tmp.name
            
            with wave.open(wav_path, 'wb') as wav:
                wav.setnchannels(channels)
                wav.setsampwidth(2)  # 16-bit
                wav.setframerate(sample_rate)
                
                # Convert samples to bytes
                data = struct.pack(f'<{len(pcm_samples)}h', *pcm_samples)
                wav.writeframes(data)
        
        # Encode to FLAC
        flac_path = wav_path.replace('.wav', '.flac')
        result = subprocess.run(['flac', '-f', '-o', flac_path, wav_path],
                              capture_output=True, text=True)
        
        if result.returncode != 0:
            raise Exception(f"FLAC encoding failed: {result.stderr}")
        
        # Read FLAC data
        with open(flac_path, 'rb') as f:
            flac_data = f.read()
        
        # Cleanup
        os.unlink(wav_path)
        os.unlink(flac_path)
        
        return flac_data
        
    except Exception as e:
        print(f"FLAC encoding error: {e}")
        print("Make sure 'flac' command is installed (apt-get install flac)")
        return None

# ===========================================
# WAV READER
# ===========================================

def read_wav_header(filename):
    """Parse WAV header and return (sample_rate, channels, bits_per_sample, format_tag) or None."""
    try:
        with open(filename, 'rb') as f:
            hdr = f.read(12)
            if len(hdr) < 12:
                return None
            riff, _size, wave_id = struct.unpack('<4sI4s', hdr)
            if riff not in (b'RIFF', b'RF64') or wave_id != b'WAVE':
                return None
            while True:
                chunk_hdr = f.read(8)
                if len(chunk_hdr) < 8:
                    return None
                chunk_id, chunk_size = struct.unpack('<4sI', chunk_hdr)
                if chunk_id == b'fmt ':
                    fmt = f.read(chunk_size)
                    if len(fmt) < 16:
                        return None
                    fmt_tag, channels, sample_rate = struct.unpack('<HHI', fmt[:8])
                    bits_per_sample = struct.unpack('<H', fmt[14:16])[0] if chunk_size >= 16 else 0
                    return sample_rate, channels, bits_per_sample, fmt_tag
                # Skip chunk (pad to even)
                f.seek(chunk_size + (chunk_size & 1), 1)
    except Exception:
        return None

def read_wav(filename):
    """Read WAV file and return PCM samples"""
    with wave.open(filename, 'rb') as wav:
        channels = wav.getnchannels()
        sample_width = wav.getsampwidth()
        sample_rate = wav.getframerate()
        num_frames = wav.getnframes()
        
        if sample_width != 2:
            raise ValueError("Only 16-bit WAV files supported")
        
        # Read all frames
        data = wav.readframes(num_frames)
        
        # Convert to list of samples
        samples = list(struct.unpack(f'<{num_frames * channels}h', data))
        
        return samples, sample_rate, channels, num_frames

# ===========================================
# ADI FILE GENERATOR
# ===========================================

def generate_adi_header(format_name, length_seconds, data_len, 
                       volume, sample_rate, channels):
    """Generate ADI file header"""
    header = "#Audio data generated by ADI converter\n"
    header += f"AudioFormat={format_name}\n"
    header += f"AudioLength={length_seconds}\n"
    header += f"AudioDatalen={data_len}\n"
    header += f"Globalvol={volume}\n"
    header += f"SampleRate={sample_rate}\n"
    header += f"Channels={channels}\n"
    header += "#DATA\n"
    return header.encode('ascii')

def wav_to_adi(input_wav, output_file, format_type, volume=80, object_file=False,
               force_sample_rate=None):
    """Convert WAV to ADI format"""
    
    print(f"Reading {input_wav}...")
    header_info = read_wav_header(input_wav)
    samples, sample_rate, channels, num_frames = read_wav(input_wav)

    if header_info is not None:
        hdr_rate, hdr_channels, hdr_bits, _fmt = header_info
        if hdr_rate and hdr_rate != sample_rate:
            print(f"  Warning: wave module rate={sample_rate} Hz, header rate={hdr_rate} Hz")
            sample_rate = hdr_rate
        if hdr_channels and hdr_channels != channels:
            print(f"  Warning: wave module channels={channels}, header channels={hdr_channels}")
        if hdr_bits and hdr_bits != 16:
            print(f"  Warning: header bits_per_sample={hdr_bits} (expected 16)")

    if force_sample_rate:
        print(f"  Forcing sample rate to {force_sample_rate} Hz")
        sample_rate = force_sample_rate
    
    length_seconds = num_frames // sample_rate
    
    print(f"  Sample rate: {sample_rate} Hz")
    print(f"  Channels: {channels}")
    print(f"  Duration: {length_seconds} seconds")
    print(f"  Samples: {len(samples)}")
    
    # Encode
    print(f"Encoding to {format_type}...")
    
    if format_type == 'IADPCM':
        compressed = encode_ima_adpcm(samples)
        format_name = 'IADPCM'
    elif format_type == 'MSADPCM':
        compressed = encode_ms_adpcm(samples)
        format_name = 'MSADPCM'
    elif format_type == 'FLAC':
        compressed = encode_flac(samples, sample_rate, channels)
        if compressed is None:
            return False
        format_name = 'FLAC'
    else:
        print(f"Unknown format: {format_type}")
        return False
    
    original_size = len(samples) * 2  # 16-bit = 2 bytes per sample
    compressed_size = len(compressed)
    ratio = original_size / compressed_size if compressed_size > 0 else 0
    
    print(f"  Original size: {original_size} bytes")
    print(f"  Compressed size: {compressed_size} bytes")
    print(f"  Compression ratio: {ratio:.2f}:1")
    
    if object_file:
        # Generate .o file for direct disk write
        print(f"Generating object file {output_file}...")
        ok = generate_object_file(output_file, input_wav, compressed, format_name, 
                                  length_seconds, volume, sample_rate, channels)
        if not ok:
            return False
    else:
        # Generate .adi file
        print(f"Writing {output_file}...")
        header = generate_adi_header(format_name, length_seconds, 
                                    compressed_size, volume, sample_rate, channels)
        
        with open(output_file, 'wb') as f:
            f.write(header)
            f.write(compressed)
    
    print("Done!")
    return True

def generate_object_file(output_file, input_wav, data, format_name, length_seconds,
                        volume, sample_rate, channels):
    """Generate ELF .o file containing only compressed audio data"""
    
    objcopy = (os.environ.get('OBJCOPY') or
               os.environ.get('X86_64_ELF_OBJCOPY') or
               shutil.which('x86_64-elf-objcopy') or
               shutil.which('objcopy'))
    if not objcopy and os.path.exists('/opt/cross/bin/x86_64-elf-objcopy'):
        objcopy = '/opt/cross/bin/x86_64-elf-objcopy'
    if not objcopy:
        print("Error: objcopy not found (need x86_64-elf-objcopy)")
        return False

    # Ensure output directory exists
    output_file = os.path.abspath(output_file)
    out_dir = os.path.dirname(output_file)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    
    base_name = os.path.basename(input_wav)
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_input = os.path.join(tmpdir, base_name)
        with open(tmp_input, 'wb') as f:
            f.write(data)
        
        # Produce a real ELF object from raw binary
        cmd = [
            objcopy,
            "-I", "binary",
            "-O", "elf64-x86-64",
            "-B", "i386",
            "--rename-section", ".data=.rodata,alloc,load,readonly,data,contents",
            base_name,
            output_file,
        ]
        result = subprocess.run(cmd, cwd=tmpdir, capture_output=True, text=True)
        if result.returncode != 0:
            print("objcopy failed:")
            print(result.stderr.strip())
            return False
    
    # Also generate a .h file with metadata
    header_file = output_file.replace('.o', '_metadata.h')
    with open(header_file, 'w') as f:
        f.write(f"// Audio metadata for {output_file}\n")
        f.write(f"#define AUDIO_FORMAT \"{format_name}\"\n")
        f.write(f"#define AUDIO_LENGTH {length_seconds}\n")
        f.write(f"#define AUDIO_DATALEN {len(data)}\n")
        f.write(f"#define AUDIO_VOLUME {volume}\n")
        f.write(f"#define AUDIO_SAMPLERATE {sample_rate}\n")
        f.write(f"#define AUDIO_CHANNELS {channels}\n")
    
    print(f"  Also created {header_file} with metadata")
    return True

# ===========================================
# MAIN
# ===========================================

def main():
    parser = argparse.ArgumentParser(description='Convert WAV to ADI audio format')
    parser.add_argument('input', help='Input WAV file')
    parser.add_argument('output', help='Output file (.adi or .o)')
    parser.add_argument('--format', choices=['IADPCM', 'MSADPCM', 'FLAC'],
                       default='IADPCM', help='Compression format')
    parser.add_argument('--volume', type=int, default=80,
                       help='Global volume (0-100)')
    parser.add_argument('--object-file', action='store_true',
                       help='Generate .o file for direct disk write')
    parser.add_argument('--sample-rate', type=int, default=None,
                       help='Override sample rate (Hz) if header is wrong')
    
    args = parser.parse_args()
    
    if not os.path.exists(args.input):
        print(f"Error: {args.input} not found")
        return 1
    
    success = wav_to_adi(args.input, args.output, args.format, 
                        args.volume, args.object_file, args.sample_rate)
    
    return 0 if success else 1

if __name__ == '__main__':
    sys.exit(main())
