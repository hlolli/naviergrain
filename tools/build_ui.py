"""Bundle the shared browser/desktop interface; no server needed by desktop."""
import argparse
from pathlib import Path
import shutil
import subprocess
import base64
import re
import json

ROOT = Path(__file__).resolve().parents[1]

def build(output, header=None, bun="bun"):
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run([bun, "build", str(ROOT / "ui/main.js"), "--target=browser",
                    "--format=iife", "--outfile", str(output / "app.js")], check=True)
    for name in ("live.worker.js", "audio.worklet.js"):
        shutil.copyfile(ROOT / "ui" / name, output / name)
    (output / "licenses").mkdir(exist_ok=True)
    for name in ("LICENSE", "COPYRIGHT"):
        shutil.copyfile(ROOT / name, output / "licenses" / name)
    for path in (ROOT / "ui/fonts").glob("*-OFL.txt"):
        shutil.copyfile(path, output / "licenses" / path.name)
    fonts = (ROOT / "ui/fonts.css").read_text()
    fonts = re.sub(r"url\((fonts/[^)]+)\)", lambda m: "url(data:font/ttf;base64," +
                   base64.b64encode((ROOT / "ui" / m[1]).read_bytes()).decode() + ")", fonts)
    css = fonts + (ROOT / "tokens.css").read_text() + (ROOT / "ui/style.css").read_text()
    page = (ROOT / "ui/index.html").read_text().replace("<!--STYLE-->", "<style>" + css + "</style>")
    (output / "index.html").write_text(page.replace("<!--SCRIPT-->", '<script src="app.js"></script>'))
    if header:
        # Fixed source delimiter cannot collide silently with edited UI code.
        embedded = page.replace("<!--SCRIPT-->", "<script>" + (output / "app.js").read_text() + "</script>")
        if ')FGHTML"' in embedded:
            raise ValueError("UI conflicts with the C++ embedding delimiter")
        header.parent.mkdir(parents=True, exist_ok=True)
        profiles=json.loads((ROOT / 'ui/about.json').read_text())['links']
        urls=','.join(json.dumps(p['url']) for p in profiles)
        header.write_text('static const char fg_ui_html[] = R"FGHTML(' + embedded + ')FGHTML";\n' +
                          f'static const char *const fg_profile_urls[] = {{{urls}}};\n' +
                          f'static constexpr unsigned fg_profile_count = {len(profiles)};\n')

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/ui")
    parser.add_argument("--header", type=Path)
    parser.add_argument("--bun", default="bun")
    args = parser.parse_args()
    build(args.output, args.header, args.bun)
