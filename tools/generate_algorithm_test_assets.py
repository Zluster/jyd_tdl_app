#!/usr/bin/env python3
import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

RESAMPLING = getattr(Image, "Resampling", Image)


def load_font(path: str, size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(path, size=size)


def fit_image(image: Image.Image, size: tuple[int, int]) -> Image.Image:
    copy = image.convert("RGB")
    copy.thumbnail(size, RESAMPLING.LANCZOS)
    return copy


def create_ocr_card(output: Path, font_path: str) -> None:
    image = Image.new("RGB", (1920, 1080), "#f4f0e6")
    draw = ImageDraw.Draw(image)
    title = load_font(font_path, 118)
    large = load_font(font_path, 86)
    medium = load_font(font_path, 64)
    small = load_font(font_path, 44)

    draw.rectangle((0, 0, 1920, 150), fill="#12372a")
    draw.text((75, 12), "CV184X  PP-OCR  TEST", font=title,
              fill="#f6c453")
    draw.rounded_rectangle((70, 205, 1850, 945), radius=28,
                           fill="#ffffff", outline="#12372a", width=5)
    draw.text((130, 235), "中文文本识别测试", font=large, fill="#111111")
    draw.text((130, 365), "设备编号：AF991A-2026", font=large,
              fill="#0b4f6c")
    draw.text((130, 495), "Hardware VPSS + BMRT", font=large,
              fill="#a23e2a")
    draw.text((130, 625), "功能测试 / 性能测试 / 文字检测", font=medium,
              fill="#222222")
    draw.text((130, 735), "0123456789  ABCDEFG  cvitek", font=medium,
              fill="#166534")

    rotated = Image.new("RGBA", (920, 100), (255, 255, 255, 0))
    rotated_draw = ImageDraw.Draw(rotated)
    rotated_draw.rounded_rectangle((0, 0, 915, 95), radius=16,
                                   fill="#fff7cc", outline="#d97706",
                                   width=3)
    rotated_draw.text((25, 16), "Rotated text  旋转文字  15 DEG",
                      font=small, fill="#7c2d12")
    rotated = rotated.rotate(7, expand=True, resample=RESAMPLING.BICUBIC)
    image.paste(rotated, (875, 830), rotated)
    draw.text((70, 1010), "Expected: text boxes align with every line",
              font=small, fill="#4b5563")
    image.save(output / "ocr_test_card.jpg", quality=95, subsampling=0)


def create_query_card(source: Path, output: Path, label: str,
                      font_path: str) -> None:
    image = Image.new("RGB", (1280, 900), "#e8e3d6")
    draw = ImageDraw.Draw(image)
    title = load_font(font_path, 72)
    subtitle = load_font(font_path, 38)
    content = fit_image(Image.open(source), (1120, 690))
    left = (image.width - content.width) // 2
    top = 125 + (690 - content.height) // 2
    image.paste(content, (left, top))
    draw.rectangle((0, 0, 1280, 110), fill="#1f3a2e")
    draw.text((55, 10), f"SELF-LEARNING QUERY: {label}", font=title,
              fill="#f8d477")
    draw.text((55, 842), "Expected top-1 label matches the displayed object",
              font=subtitle, fill="#374151")
    image.save(output, quality=95, subsampling=0)


def create_tracking_guide(output: Path, font_path: str) -> None:
    image = Image.new("RGB", (1280, 720), "#eef2e5")
    draw = ImageDraw.Draw(image)
    title = load_font(font_path, 72)
    body = load_font(font_path, 50)
    small = load_font(font_path, 38)
    draw.rectangle((0, 0, 1280, 105), fill="#173f35")
    draw.text((45, 10), "ByteTrack 现场功能测试", font=title,
              fill="#f4c95d")
    draw.line((640, 135, 640, 670), fill="#eab308", width=12)
    draw.polygon([(570, 325), (470, 270), (470, 310), (180, 310),
                  (180, 340), (470, 340), (470, 380)], fill="#2563eb")
    draw.polygon([(710, 325), (810, 270), (810, 310), (1100, 310),
                  (1100, 340), (810, 340), (810, 380)], fill="#dc2626")
    draw.text((100, 420), "人员 A：从左向右穿线", font=body,
              fill="#1e3a8a")
    draw.text((720, 500), "人员 B：从右向左穿线", font=body,
              fill="#7f1d1d")
    draw.text((90, 610), "检查 ID 稳定、轨迹连续、L->R / R->L 各加 1",
              font=small, fill="#111827")
    image.save(output / "bytetrack_camera_test_guide.jpg", quality=95,
               subsampling=0)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--assets-dir", required=True)
    parser.add_argument("--font", default=(
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"))
    args = parser.parse_args()
    assets = Path(args.assets_dir)
    assets.mkdir(parents=True, exist_ok=True)
    create_ocr_card(assets, args.font)
    create_query_card(assets / "dog.jpg",
                      assets / "self_learning_dog_query.jpg", "DOG",
                      args.font)
    create_query_card(assets / "plant.jpg",
                      assets / "self_learning_plant_query.jpg", "PLANT",
                      args.font)
    create_tracking_guide(assets, args.font)
    print("generated_assets=4")


if __name__ == "__main__":
    main()
