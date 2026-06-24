# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
"""
Generate one SVG icon by asking the Google Gemini API for SVG markup.

Usage:
    python code-miscellaneous/generate_svg_icon.py --id=57345 --text="Polyline" --apikey=<Gemini API Key>

The generated SVG is written to website/static/SVGIcons by default.
"""

import argparse
import json
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

from pathlib import Path


SVGDescription = ""
SVGIconID = 0
SVGGuideLines = [
    """Generate an valid SVG Icon with  viewBox="0 0 64 64"  """,
    "Professional looking, Minimalist, RGB",
    "Shall be used as Icons in Various Screen Density display, rasterized at runtime",
    "No raster images, scripts, animations, embedded objects, external links, fonts, or CSS imports.",
    "Keep the SVG text in pretty-printed format, with indentation and line breaks.",
]
GeminiAPIKey = "Do-Not-Commit-API-KEY-to-GIT" #"Do-Not-Commit-API-KEY-to-GIT"
GeminiModel = "gemini-3.5-flash" #Other examples: "gemini-3.1-flash-lite"

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
OUTPUT_DIR = PROJECT_ROOT / "website" / "static" / "SVGIcons"

DISALLOWED_SVG_TAGS = {
    "animate",
    "animateMotion",
    "animateTransform",
    "discard",
    "embed",
    "foreignObject",
    "iframe",
    "image",
    "object",
    "script",
    "set",
    "video",
}


def info(message: str):
    print(f"[INFO] {message}")


def error(message: str):
    print(f"[ERROR] {message}", file=sys.stderr)


def strip_namespace(tag: str) -> str:
    return tag.split("}")[-1]


def slugify(text: str) -> str:
    slug = re.sub(r"[^A-Za-z0-9]+", "_", text.strip()).strip("_").lower()
    return slug or "icon"


def parse_uint32(value: str) -> int:
    try:
        parsed_value = int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("--id must be a 32 bit number.") from exc

    if parsed_value < 0 or parsed_value > 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("--id must be between 0 and 4294967295.")

    return parsed_value


def build_prompt(description: str) -> str:
    guidelines = "\n".join(f"- {line}" for line in SVGGuideLines)
    return f"""Create an SVG icon for this action:
{description}

Guidelines:
{guidelines}

Strict output rules:
- Return only raw SVG markup. Do not wrap it in Markdown or code fences.
- The root element must be <svg>.
- The root element must include xmlns="http://www.w3.org/2000/svg".
- The root element must include exactly viewBox="0 0 64 64".
- Use vector SVG elements only. Do not use raster images, scripts, animations, embedded objects, external links, fonts, or CSS imports.
- Prefer simple paths, polylines, lines, rectangles, circles, polygons, and RGB colors.
- Keep the icon clear when rasterized at small screen densities.
"""


def make_gemini_request(api_key: str, model: str, prompt: str) -> dict:
    encoded_model = urllib.parse.quote(model, safe="")
    endpoint = (
        "https://generativelanguage.googleapis.com/v1beta/models/"
        f"{encoded_model}:generateContent?key={urllib.parse.quote(api_key, safe='')}"
    )
    payload = {
        "systemInstruction": {
            "parts": [
                {
                    "text": (
                        "You generate compact, production-ready SVG icons. "
                        "Your response must be only one valid SVG document."
                    )
                }
            ]
        },
        "contents": [
            {
                "role": "user",
                "parts": [{"text": prompt}],
            }
        ],
        "generationConfig": {
            "candidateCount": 1,
            "maxOutputTokens": 4096,
            "temperature": 0.2,
            "topP": 0.9,
            "responseMimeType": "text/plain",
        },
    }

    request = urllib.request.Request(
        endpoint,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as http_error:
        body = http_error.read().decode("utf-8", errors="replace")
        raise RuntimeError(
            f"Gemini API returned HTTP {http_error.code}: {body}"
        ) from http_error
    except urllib.error.URLError as url_error:
        raise RuntimeError(f"Gemini API request failed: {url_error}") from url_error
    except json.JSONDecodeError as json_error:
        raise RuntimeError(f"Gemini API returned invalid JSON: {json_error}") from json_error


def extract_text_from_response(response_json: dict) -> str:
    candidates = response_json.get("candidates", [])
    if not candidates:
        prompt_feedback = response_json.get("promptFeedback")
        raise RuntimeError(f"Gemini API returned no candidates: {prompt_feedback}")

    text_parts = []
    for part in candidates[0].get("content", {}).get("parts", []):
        text = part.get("text")
        if text:
            text_parts.append(text)

    if not text_parts:
        raise RuntimeError(f"Gemini API returned no text parts: {response_json}")

    return "\n".join(text_parts).strip()


def extract_svg(response_text: str) -> str:
    cleaned = response_text.strip()
    fence_match = re.search(r"```(?:svg|xml)?\s*(.*?)```", cleaned, re.DOTALL | re.IGNORECASE)
    if fence_match:
        cleaned = fence_match.group(1).strip()

    svg_match = re.search(r"<svg\b.*?</svg>", cleaned, re.DOTALL | re.IGNORECASE)
    if not svg_match:
        raise RuntimeError("Gemini response did not contain a complete <svg>...</svg> document.")

    return svg_match.group(0).strip()


def validate_svg(svg_text: str):
    try:
        root = ET.fromstring(svg_text)
    except ET.ParseError as parse_error:
        raise RuntimeError(f"Generated SVG is not valid XML: {parse_error}") from parse_error

    if strip_namespace(root.tag) != "svg":
        raise RuntimeError("Generated SVG root element is not <svg>.")

    if root.attrib.get("viewBox") != "0 0 64 64":
        raise RuntimeError('Generated SVG must include viewBox="0 0 64 64".')

    for element in root.iter():
        tag = strip_namespace(element.tag)
        if tag in DISALLOWED_SVG_TAGS:
            raise RuntimeError(f"Generated SVG contains disallowed <{tag}> element.")

        for attribute_name, attribute_value in element.attrib.items():
            lower_name = attribute_name.lower()
            lower_value = attribute_value.strip().lower()
            if lower_name.startswith("on"):
                raise RuntimeError(
                    f"Generated SVG contains event handler attribute '{attribute_name}'."
                )
            if "href" in lower_name:
                raise RuntimeError(
                    f"Generated SVG contains link attribute '{attribute_name}'."
                )
            if "url(" in lower_value and ("http:" in lower_value or "https:" in lower_value):
                raise RuntimeError("Generated SVG contains an external URL reference.")
            if "javascript:" in lower_value:
                raise RuntimeError("Generated SVG contains a javascript URL.")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Generate an SVG icon using Google Gemini API."
    )
    parser.add_argument(
        "--id",
        required=True,
        type=parse_uint32,
        help="Unsigned 32 bit icon ID used in the output filename.",
    )
    parser.add_argument(
        "--text",
        required=True,
        help='Action/icon description, for example --text="Polyline".',
    )
    parser.add_argument(
        "--apikey",
        default=None,
        help="Gemini API key. Falls back to the embedded GeminiAPIKey value when omitted.",
    )
    parser.add_argument(
        "--model",
        default=GeminiModel,
        help=f"Gemini model to call. Default: {GeminiModel}",
    )
    parser.add_argument(
        "--output-dir",
        default=str(OUTPUT_DIR),
        help=f"Output directory. Default: {OUTPUT_DIR}",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Overwrite the output file if it already exists.",
    )
    return parser.parse_args()


def main() -> int:
    global SVGDescription
    global SVGIconID

    args = parse_args()
    SVGIconID = args.id
    SVGDescription = args.text.strip()
    if not SVGDescription:
        error("--text must not be empty.")
        return 1

    api_key = args.apikey or GeminiAPIKey
    if not api_key or api_key == "Do-Not-Commit-API-KEY-to-GIT":
        error(
            "Gemini API key is missing. Pass --apikey=<Gemini API Key> "
            "or replace the embedded GeminiAPIKey placeholder locally."
        )
        return 1

    output_dir = Path(args.output_dir).resolve()
    output_filename = f"icon_{SVGIconID}_{slugify(SVGDescription)}.svg"
    output_path = output_dir / output_filename

    if output_path.exists() and not args.overwrite:
        error(f"Output file already exists: {output_path}")
        error("Use --overwrite to replace it.")
        return 1

    output_dir.mkdir(parents=True, exist_ok=True)

    prompt = build_prompt(SVGDescription)
    info(f"Requesting SVG icon from Gemini model: {args.model}")

    try:
        response_json = make_gemini_request(api_key, args.model, prompt)
        response_text = extract_text_from_response(response_json)
        svg_text = extract_svg(response_text)
        validate_svg(svg_text)
        output_path.write_text(svg_text + "\n", encoding="utf-8")
    except Exception as exc:
        error(str(exc))
        return 1

    info(f"SVG icon written to: {output_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
