"""按目标像素网格生成小尺寸图标，ICO 中保留独立帧以避免反复缩放。"""
from pathlib import Path
from io import BytesIO
import struct
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "assets/app-icon"
SIZES = (16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 96, 128, 256)

def frame(size):
    # 从几何轮廓重新绘制：橙色后页、黄色前页，画布始终透明。
    scale = 4
    image = Image.new("RGBA", (size * scale, size * scale))
    draw = ImageDraw.Draw(image)
    def point(x, y):
        return (round((.5 + (x - .5) * 1.125) * size) * scale,
                round((.5 + (y - .5) * 1.125) * size) * scale)
    def bounds(x0, y0, x1, y1):
        return (*point(x0, y0), *point(x1, y1))
    orange, yellow = "#FFA000", "#FFCE2B"
    radius = max(1, round(size * 1.125 / 12)) * scale
    draw.rounded_rectangle(bounds(1/12, 1/6, 5/12, 5/6),
                           radius=radius, fill=orange)
    draw.polygon([point(1/6, 1/6), point(3/8, 1/6),
                  point(11/24, 1/4), point(11/24, 5/6),
                  point(1/6, 5/6)], fill=orange)
    draw.rounded_rectangle(bounds(1/12, 1/4, 11/12, 5/6),
                           radius=radius, fill=yellow)
    image = image.resize((size, size), Image.Resampling.LANCZOS)
    # 清除重采样在透明区域产生的极低透明度杂点。
    image.putalpha(image.getchannel("A").point(lambda a: 0 if a < 4 else a))
    return image


def main():
    frame(256).save(DEST / "zenexplorer.png")
    payloads=[]
    for size in SIZES:
        buffer=BytesIO()
        frame(size).save(buffer,format="PNG")
        payloads.append(buffer.getvalue())
    offset=6+16*len(SIZES)
    directory=bytearray(struct.pack("<HHH",0,1,len(SIZES)))
    for size,payload in zip(SIZES,payloads):
        directory.extend(struct.pack("<BBBBHHII",size if size<256 else 0,
            size if size<256 else 0,0,0,1,32,len(payload),offset))
        offset+=len(payload)
    (DEST/"zenexplorer.ico").write_bytes(directory+b"".join(payloads))

if __name__ == "__main__":
    main()
