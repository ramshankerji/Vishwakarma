import json
import sys
from PIL import Image

def get_float(obj, key, default=0.0):
    return float(obj.get(key, default))

def cpp_float(value):
    text = f"{float(value):.9g}"
    if "." not in text and "e" not in text and "E" not in text:
        text += ".0"
    return f"{text}f"

def generate_header(png_path, json_path, out_path):
    # 1. Read Image and convert to RGBA
    img = Image.open(png_path).convert('RGBA')
    width, height = img.size
    pixels = list(img.getdata())
    
    # 2. Read JSON
    with open(json_path, 'r', encoding='utf-8') as f:
        data = json.load(f)
        
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write("// AUTO-GENERATED MSDF FONT HEADER\n")
        f.write("#pragma once\n")
        f.write("#include <cstdint>\n")
        f.write("#include <unordered_map>\n\n")
        
        f.write(f"inline constexpr int NotoSansMSDF_Width = {width};\n")
        f.write(f"inline constexpr int NotoSansMSDF_Height = {height};\n\n")
        atlas_info = data.get('atlas', {})
        metrics = data.get('metrics', {})
        f.write(f"inline constexpr float NotoSansMSDF_Size = {cpp_float(get_float(atlas_info, 'size', 32.0))};\n")
        f.write(f"inline constexpr float NotoSansMSDF_PxRange = {cpp_float(get_float(atlas_info, 'distanceRange', 4.0))};\n")
        f.write(f"inline constexpr float NotoSansMSDF_Ascender = {cpp_float(get_float(metrics, 'ascender', 0.0))};\n")
        f.write(f"inline constexpr float NotoSansMSDF_Descender = {cpp_float(get_float(metrics, 'descender', 0.0))};\n")
        f.write(f"inline constexpr float NotoSansMSDF_LineHeight = {cpp_float(get_float(metrics, 'lineHeight', 1.0))};\n\n")
        
        # 3. Write Pixel Data
        f.write("inline const uint8_t NotoSansMSDF_Pixels[] = {\n")
        byte_array = [b for pixel in pixels for b in pixel]
        
        lines = []
        for i in range(0, len(byte_array), 16):
            chunk = byte_array[i:i+16]
            lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
        f.write("\n".join(lines))
        f.write("\n};\n\n")
        
        # 4. Write Glyph Metrics
        f.write("struct MSDFGlyph {\n")
        f.write("    float advance;\n")
        f.write("    float planeLeft, planeBottom, planeRight, planeTop;\n")
        f.write("    float atlasLeft, atlasBottom, atlasRight, atlasTop;\n")
        f.write("};\n\n")
        
        f.write("inline std::unordered_map<char32_t, MSDFGlyph> NotoSansMSDF_Glyphs = {\n")
        
        for g in data.get('glyphs', []):
            # SAFEGUARD: Skip internal glyph-index-only entries that have no Unicode mapping
            if 'unicode' not in g:
                continue
                
            uni = int(g['unicode'])
            adv = g.get('advance', 0.0)
            
            # SAFEGUARD: Provide 0,0,0,0 bounds for whitespace characters like "Space"
            pb = g.get('planeBounds', {'left':0.0, 'bottom':0.0, 'right':0.0, 'top':0.0})
            ab = g.get('atlasBounds', {'left':0.0, 'bottom':0.0, 'right':0.0, 'top':0.0})
            
            values = [adv, pb['left'], pb['bottom'], pb['right'], pb['top'],
                      ab['left'], ab['bottom'], ab['right'], ab['top']]
            f.write(f"    {{ {uni}, {{ {', '.join(cpp_float(v) for v in values)} }} }},\n")
        
        f.write("};\n")

if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("Usage: msdf_atlas_json_parser.py <png> <json> <out_header>")
        sys.exit(1)
    generate_header(sys.argv[1], sys.argv[2], sys.argv[3])
