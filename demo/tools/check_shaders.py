"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.
Clamp's check (galvo's shape), for a plugin whose shaders are SNIPPETS.

------------------------------------------------------------------- why

`source/Shaders.cpp` holds twenty GLSL string constants and joins them into
ten programs; `demo/plugin.js` holds the same twenty as template literals and
joins them the same way. That is two copies of the same text, and two copies
drift -- quietly, because a demo that renders a *plausible* picture looks
exactly like a demo that renders the right one. The whole claim of these pages
is that they run the plugin's own shader rather than something reimplemented
to look similar, so the claim needs something enforcing it.

Nothing else can. `actest` drives the real plugin class through a real FFGL
sequence and has no idea this page exists, and `tools/check-shaders.sh`
compiles the C++ copies and never looks at the JS one.

------------------------------------------------------------------- what it does

Pulls each `R"( ... )"` body out of the C++ and each matching backtick literal
out of `plugin.js`, and compares them exactly -- no whitespace normalisation, no
comment stripping. A comment that has been updated on one side and not the other
is exactly the drift worth catching, because comments in this repo carry the
reasoning that justifies the code.

The one transformation is a decode, not a normalisation. A backtick cannot
appear raw inside a JavaScript template literal, so `plugin.js` escapes the one
the lapped snippet's comment carries (`The block \\`sub\\` of cell b`) as \\`.
This unescapes that and *rejects any other backslash on the JS side*; there are
none anywhere in the C++, so a second escape could only be somebody hiding a
difference. A `${` would be interpolated by the literal, so it is refused on the
C++ side before it can become a silent difference.

It also checks the JOIN: that plugin.js assembles the six compound programs
from the same snippets in the same order as Shaders.cpp's join() calls.

------------------------------------------------------------------- what it cannot

Nothing here checks the *ported* half. The tables (Codec.cpp), the disc
(Disc.cpp), the Clock, every conversion in Controls.cpp and the frame sequence
of Atrac::ProcessOpenGL are a hand translation in plugin.js, and only a reader
can tell whether they still agree. When you change one of those, change it here
too -- and remember that a wrong mapping shows up on the page as a picture that
is subtly wrong, which nobody will notice.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# JS constant, C++ symbol. Every string constant in Shaders.cpp, in its order.
SNIPPETS = [
    ("VERTEX", "kVertexSource"),
    ("CONVERT", "kConvertSource"),
    ("CELLS", "kCellsSource"),
    ("LAPPED", "kLappedSource"),
    ("FORWARD_BODY", "kForwardBody"),
    ("MDCTX_HEAD", "kMdctXHead"),
    ("MDCTX_HELPERS", "kMdctXHelpers"),
    ("MDCTY_HEAD", "kMdctYHead"),
    ("MDCTY_HELPERS", "kMdctYHelpers"),
    ("INVERSE_BODY", "kInverseBody"),
    ("IMDCTY_HEAD", "kImdctYHead"),
    ("IMDCTY_HELPERS", "kImdctYHelpers"),
    ("IMDCTX_HEAD", "kImdctXHead"),
    ("IMDCTX_HELPERS", "kImdctXHelpers"),
    ("ALLOC_READ", "kAllocReadSource"),
    ("ALLOC", "kAllocSource"),
    ("QUANT_HEAD", "kQuantHead"),
    ("QUANT_BODY", "kQuantBody"),
    ("DISPLAY_HEAD", "kDisplayHead"),
    ("DISPLAY_BODY", "kDisplayBody"),
]
CPP_TO_JS = {cpp: js for js, cpp in SNIPPETS}

# The six programs Shaders.cpp joins, as plugin.js must join them.
JOINS = {
    "MDCTX": ["MDCTX_HEAD", "LAPPED", "MDCTX_HELPERS", "FORWARD_BODY"],
    "MDCTY": ["MDCTY_HEAD", "LAPPED", "MDCTY_HELPERS", "FORWARD_BODY"],
    "QUANT": ["QUANT_HEAD", "ALLOC_READ", "QUANT_BODY"],
    "IMDCTY": ["IMDCTY_HEAD", "LAPPED", "IMDCTY_HELPERS", "INVERSE_BODY"],
    "IMDCTX": ["IMDCTX_HEAD", "LAPPED", "IMDCTX_HELPERS", "INVERSE_BODY"],
    "DISPLAY": ["DISPLAY_HEAD", "ALLOC_READ", "DISPLAY_BODY"],
}


def from_cpp(source, symbol):
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    return None if match is None else match.group(1)


def cpp_joins(source):
    """Shaders.cpp's join( { a, b, c } ) calls, by the function that returns them."""
    out = {}
    for match in re.finditer(r'const std::string& (\w+)\(\)\s*\{\s*static const std::string s = join\( \{ ([^}]*) \} \);', source, re.S):
        parts = [p.strip() for p in match.group(2).split(",")]
        out[match.group(1)] = [CPP_TO_JS.get(p, p) for p in parts]
    return out


def from_js(source, name):
    match = re.search(r'^const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not an escaped backtick, at line {upto.count(chr(10)) + 1}"
    return body.replace("\\`", "`"), None


def js_joins(source):
    out = {}
    for match in re.finditer(r'^const (\w+) = ((?:\w+ \+ )+\w+);$', source, re.M):
        out[match.group(1)] = [p.strip() for p in match.group(2).split("+")]
    return out


def main():
    with open(os.path.join(REPO, "source", "Shaders.cpp")) as handle:
        cpp = handle.read()
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()

    problems = 0
    for name, symbol in SNIPPETS:
        cpp_text = from_cpp(cpp, symbol)
        js_text, complaint = from_js(js, name)

        if cpp_text is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp")
            problems += 1
            continue
        if "${" in cpp_text:
            print(f"FAIL  {symbol} contains ${{, which a template literal would interpolate")
            problems += 1
            continue
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue
        if cpp_text == js_text:
            print(f"ok    {name:<16} matches {symbol} ({len(cpp_text)} chars)")
            continue

        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in source/Shaders.cpp")
        cpp_lines = cpp_text.splitlines()
        js_lines = js_text.splitlines()
        for i in range(max(len(cpp_lines), len(js_lines))):
            a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
            b = js_lines[i] if i < len(js_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break

    # The constants in Shaders.cpp that this table does not know are drift too.
    known = {symbol for _, symbol in SNIPPETS}
    for symbol in re.findall(r'const char\* const (k\w+) = R"\(', cpp):
        if symbol not in known:
            print(f"FAIL  {symbol} is in source/Shaders.cpp but not in this check's table")
            problems += 1

    # The joins.
    expected = cpp_joins(cpp)
    actual = js_joins(js)
    name_map = {"MdctX": "MDCTX", "MdctY": "MDCTY", "Quant": "QUANT",
                "ImdctY": "IMDCTY", "ImdctX": "IMDCTX", "Display": "DISPLAY"}
    for fn, parts in expected.items():
        js_name = name_map.get(fn)
        if js_name is None:
            print(f"FAIL  Shaders.cpp joins {fn}(), which this check does not map")
            problems += 1
            continue
        if actual.get(js_name) != parts:
            print(f"FAIL  {js_name} is joined as {actual.get(js_name)}, Shaders.cpp's {fn}() joins {parts}")
            problems += 1
        else:
            print(f"ok    {js_name:<16} joined as {' + '.join(parts)}")
    if set(JOINS) != set(name_map.values()):
        print("FAIL  the JOINS table and the name map disagree")
        problems += 1

    print()
    if problems:
        print(f"{problems} problem(s) -- copy the C++ across, do not edit plugin.js by hand")
        return 1
    print(f"all {len(SNIPPETS)} snippets are identical to the plugin's, and the {len(expected)} joins match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
