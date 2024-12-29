# Prerequisite to use this file on windows: Install MSYS2 using following steps.
# 1. Download the installer from https://www.msys2.org/ and install it.
# 2. Start MSYS2 shell "MSYS2 MinGW 64-bit" and update all packages using command "pacman -Syu". I prompted, restart shell & update again.
# 3. Install cairo using command "pacman -S mingw-w64-x86_64-cairo" in the Terminal.
# 4. (Optional) Install GCC using ""
# 5. Add MSYS2 binary to PATH environment variable. "C:\msys64\mingw64\bin" or your own directory.
# 6. Install cairosvg python library by giving following command in command prompt. "pip3 install cairosvg".

import cairosvg
from PIL import Image
import os

def convert_svg_to_ico(svg_path, ico_path=None, sizes=None):
    """
    Convert an SVG file to ICO format using CairoSVG and Pillow.

    Args:
        svg_path (str): Path to the source SVG file.
        ico_path (str, optional): Path for the output ICO file. Default: same as SVG name.
        sizes (list, optional): List of sizes for the ICO file. Default: [16, 32, 48, 64, 128, 256].
    
    Returns:
        str: Path to the created ICO file.
    """
    if sizes is None:
        sizes = [16, 32, 48, 64, 128, 256]
    
    if ico_path is None:
        ico_path = os.path.splitext(svg_path)[0] + '.ico'
    
    try:
        # Temporary PNG path for conversion
        png_path = os.path.splitext(svg_path)[0] + '.png'

        # Convert SVG to PNG using CairoSVG
        cairosvg.svg2png(url=svg_path, write_to=png_path)

        # Open the PNG file with Pillow
        img = Image.open(png_path).convert("RGBA")

        # Resize for each size and save as ICO
        icon_sizes = [(size, size) for size in sizes]
        img.save(ico_path, format="ICO", sizes=icon_sizes)

        # Clean up temporary PNG file
        os.remove(png_path)
        
        return ico_path

    except Exception as e:
        raise Exception(f"Error converting SVG to ICO: {e}")

# Example usage
if __name__ == "__main__":
    try:
        result = convert_svg_to_ico("logo.svg")
        print(f"Successfully converted to: {result}")
    except Exception as e:
        print(f"Error: {e}")
