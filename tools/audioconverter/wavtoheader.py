#!/usr/bin/env python3
"""WAV to C Header Converter (and optional .o generator)"""
import sys, struct, os, subprocess, re, tempfile

def read_wav(filename):
    with open(filename, 'rb') as f:
        data = f.read()
    if data[0:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise ValueError("Not a valid WAV file")
    fmt_offset = data.find(b'fmt ')
    audio_format = struct.unpack('<H', data[fmt_offset+8:fmt_offset+10])[0]
    num_channels = struct.unpack('<H', data[fmt_offset+10:fmt_offset+12])[0]
    sample_rate = struct.unpack('<I', data[fmt_offset+12:fmt_offset+16])[0]
    byte_rate = struct.unpack('<I', data[fmt_offset+16:fmt_offset+20])[0]
    block_align = struct.unpack('<H', data[fmt_offset+20:fmt_offset+22])[0]
    bits_per_sample = struct.unpack('<H', data[fmt_offset+22:fmt_offset+24])[0]
    data_offset = data.find(b'data')
    data_size = struct.unpack('<I', data[data_offset+4:data_offset+8])[0]
    audio_data = data[data_offset+8:data_offset+8+data_size]
    return {'audio_format': audio_format, 'num_channels': num_channels,
            'sample_rate': sample_rate, 'byte_rate': byte_rate,
            'block_align': block_align, 'bits_per_sample': bits_per_sample,
            'data_size': data_size, 'audio_data': audio_data}

def wav_to_header(input_file, output_file):
    wav = read_wav(input_file)
    base_name = os.path.splitext(os.path.basename(input_file))[0]
    var_name = base_name.replace('-', '_').replace(' ', '_').lower()
    bytes_per_sample = (wav['bits_per_sample'] / 8) * wav['num_channels']
    num_samples = int(wav['data_size'] / bytes_per_sample)
    duration = num_samples / wav['sample_rate']
    bitrate = wav['byte_rate'] * 8
    audio_type = "PCM (uncompressed)" if wav['audio_format'] == 1 else f"format {wav['audio_format']}"
    
    with open(output_file, 'w') as f:
        guard = f"{var_name.upper()}_H"
        f.write(f"#ifndef {guard}\n#define {guard}\n\n#include \"audio.h\"\n\n")
        f.write(f"// {base_name}: {wav['num_channels']}ch {wav['sample_rate']}Hz {wav['bits_per_sample']}bit {duration:.2f}s\n")
        f.write(f"// Metadata: type={audio_type}, bitrate={bitrate}bps, duration={duration:.2f}s\n\n")
        f.write(f"static const wav_header_t {var_name}_header = {{\n")
        f.write(f"    {{'R','I','F','F'}}, {wav['data_size']+36}, {{'W','A','V','E'}},\n")
        f.write(f"    {{'f','m','t',' '}}, 16, {wav['audio_format']}, {wav['num_channels']},\n")
        f.write(f"    {wav['sample_rate']}, {wav['byte_rate']}, {wav['block_align']}, {wav['bits_per_sample']},\n")
        f.write(f"    {{'d','a','t','a'}}, {wav['data_size']}\n}};\n\n")
        f.write(f"static const uint8_t {var_name}_data[] = {{\n")
        for i in range(0, len(wav['audio_data']), 12):
            chunk = wav['audio_data'][i:i+12]
            f.write("    " + ', '.join(f'0x{b:02X}' for b in chunk) + ",\n")
        f.write("};\n\n")
        f.write(f"const audio_sound_t {var_name}_sound = {{\n")
        f.write(f"    \"{base_name}\", &{var_name}_header, {var_name}_data,\n")
        f.write(f"    sizeof({var_name}_data), {num_samples}, {wav['num_channels']},\n")
        f.write(f"    {wav['bits_per_sample']}, {wav['sample_rate']}, {wav['audio_format']},\n")
        f.write(f"    {bitrate}, (uint32_t)({duration * 1000:.0f})\n}};\n\n")
        f.write(f"#endif // {guard}\n")
    print(f"Created {output_file}")
    print(f"Use: extern const audio_sound_t {var_name}_sound;")
    print(f"     audio_play(&{var_name}_sound, stream, 75, false);")
    print(f"Metadata written, type: {audio_type}, bitrate {bitrate}bps, {duration:.2f}s")


def _objcopy_symbol_base(input_path):
    # objcopy uses the provided input path and replaces non-alnum with '_'
    return re.sub(r'[^0-9A-Za-z]', '_', input_path)

def wav_to_object(input_file, output_file):
    base = os.path.splitext(os.path.basename(input_file))[0]
    sym_base = _objcopy_symbol_base(input_file)
    cmd = [
        "objcopy", "-I", "binary", "-O", "elf64-x86-64", "-B", "i386:x86-64",
        "--redefine-sym", f"_binary_{sym_base}_start=_binary_{base}_wav_start",
        "--redefine-sym", f"_binary_{sym_base}_end=_binary_{base}_wav_end",
        "--redefine-sym", f"_binary_{sym_base}_size=_binary_{base}_wav_size",
        input_file, output_file,
    ]
    subprocess.check_call(cmd)
    print(f"Created {output_file}")
    print(f"Symbols: _binary_{base}_wav_start/_end/_size")
    

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python3 wav_to_header.py input.wav output.h|output.o")
        sys.exit(1)
    inp = sys.argv[1]
    out = sys.argv[2]
    if out.endswith(".o"):
        wav_to_object(inp, out)
    else:
        wav_to_header(inp, out)
