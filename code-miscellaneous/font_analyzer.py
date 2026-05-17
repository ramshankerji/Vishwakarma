# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
import os
from fontTools.ttLib import TTFont

def main():
    # Since the script is in 'code-miscellaneous' parallel to 'Fonts', 
    # we can resolve the Fonts directory path dynamically.
    script_dir = os.path.dirname(os.path.abspath(__file__))
    fonts_dir = os.path.abspath(os.path.join(script_dir, '..', 'Fonts'))
    build_dir = os.path.abspath(os.path.join(script_dir, '..', 'build'))
    
    output_filename = "alphabets-in-fonts.txt"
    output_path = os.path.join(build_dir, output_filename)
    
    # Use a list to allow duplicates between different font files
    all_characters = []
    
    print(f"Scanning directory: {fonts_dir}\n")
    print("-" * 50)
    print(f"{'Font File Name':<50} | {'Glyph Count'}")
    print("-" * 50)

    if not os.path.exists(fonts_dir):  # Check if directory exists
        print(f"Error: The directory {fonts_dir} does not exist.")
        return

    # Iterate through all files in the Fonts directory
    for filename in os.listdir(fonts_dir):
        if filename.lower().endswith(".ttf"):
            filepath = os.path.join(fonts_dir, filename)
            
            try:
                font = TTFont(filepath) # Load the font file
                
                # 1. Get and print the number of glyphs
                # The 'maxp' (Maximum Profile) table holds the number of glyphs
                num_glyphs = font['maxp'].numGlyphs
                print(f"{filename:<50} | {num_glyphs}")
                
                # 2. Extract supported characters (alphabets/symbols)
                # Use a local set to deduplicate WITHIN the current file only
                font_characters = set() 
                for table in font['cmap'].tables:
                    if table.isUnicode():
                        for codepoint in table.cmap.keys():
                            # Convert integer codepoint to actual character and add to local set
                            font_characters.add(chr(codepoint))
                
                # Sort the characters for this specific font and add to the master list
                all_characters.extend(sorted(list(font_characters)))
                            
                # Close the font to free up memory
                font.close()
                
            except Exception as e:
                print(f"{filename:<50} | Error processing file: {e}")

    print("-" * 50)
    
    # Write the characters to the output text file with a max of 120 characters per line
    try:
        with open(output_path, 'w', encoding='utf-8') as f:
            full_string = "".join(all_characters)
            
            # Step through the string 120 characters at a time
            for i in range(0, len(full_string), 120):
                f.write(full_string[i:i+120] + "\n")
                
        print(f"\nSuccess: Extracted {len(all_characters)} total characters.")
        print(f"Saved to: {output_path}")
    except Exception as e:
        print(f"\nError writing to output file: {e}")

    input("\nPress Enter to exit...")

if __name__ == "__main__":
    main()
    