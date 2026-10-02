{%- set layouts = {
    "yuv420p":      {"model": "yuv", "components": [[0, 1, 0, 0, 8], [1, 1, 0, 0, 8], [2, 1, 0, 0, 8]], "formats": "yuv420p yuv422p yuv444p yuv410p yuv411p yuvj420p yuvj422p yuvj444p yuv440p yuvj440p yuvj411p"},
    "yuyv422":      {"model": "yuv", "components": [[0, 2, 0, 0, 8], [0, 4, 1, 0, 8], [0, 4, 3, 0, 8]], "formats": "yuyv422"},
    "rgb24":        {"model": "rgb", "components": [[0, 3, 0, 0, 8], [0, 3, 1, 0, 8], [0, 3, 2, 0, 8]], "formats": "rgb24"},
    "bgr24":        {"model": "rgb", "components": [[0, 3, 2, 0, 8], [0, 3, 1, 0, 8], [0, 3, 0, 0, 8]], "formats": "bgr24"},
    "gray":         {"model": "gray", "components": [[0, 1, 0, 0, 8]], "formats": "gray"},
    "monow":        {"model": "gray", "flags": ["bits"], "inverted": true, "components": [[0, 1, 0, 0, 1]], "formats": "monow"},
    "monob":        {"model": "gray", "flags": ["bits"], "components": [[0, 1, 0, 7, 1]], "formats": "monob"},
    "pal8":         {"model": "palette", "flags": ["alpha"], "components": [[0, 1, 0, 0, 8]], "formats": "pal8"},
    "uyvy422":      {"model": "yuv", "components": [[0, 2, 1, 0, 8], [0, 4, 0, 0, 8], [0, 4, 2, 0, 8]], "formats": "uyvy422"},
    "uyyvyy411":    {"model": "yuv", "components": [[0, 4, 1, 0, 8], [0, 6, 0, 0, 8], [0, 6, 3, 0, 8]], "luma": [6, [1, 2, 4, 5]], "formats": "uyyvyy411"},
    "bgr8":         {"model": "rgb", "components": [[0, 1, 0, 0, 3], [0, 1, 0, 3, 3], [0, 1, 0, 6, 2]], "formats": "bgr8"},
    "bgr4":         {"model": "rgb", "flags": ["bits"], "components": [[0, 4, 3, 0, 1], [0, 4, 1, 0, 2], [0, 4, 0, 0, 1]], "formats": "bgr4"},
    "bgr4_byte":    {"model": "rgb", "components": [[0, 1, 0, 0, 1], [0, 1, 0, 1, 2], [0, 1, 0, 3, 1]], "formats": "bgr4_byte"},
    "rgb8":         {"model": "rgb", "components": [[0, 1, 0, 5, 3], [0, 1, 0, 2, 3], [0, 1, 0, 0, 2]], "formats": "rgb8"},
    "rgb4":         {"model": "rgb", "flags": ["bits"], "components": [[0, 4, 0, 0, 1], [0, 4, 1, 0, 2], [0, 4, 3, 0, 1]], "formats": "rgb4"},
    "rgb4_byte":    {"model": "rgb", "components": [[0, 1, 0, 3, 1], [0, 1, 0, 1, 2], [0, 1, 0, 0, 1]], "formats": "rgb4_byte"},
    "nv12":         {"model": "yuv", "components": [[0, 1, 0, 0, 8], [1, 2, 0, 0, 8], [1, 2, 1, 0, 8]], "formats": "nv12 nv16 nv24"},
    "nv21":         {"model": "yuv", "components": [[0, 1, 0, 0, 8], [1, 2, 1, 0, 8], [1, 2, 0, 0, 8]], "formats": "nv21 nv42"},
    "argb":         {"model": "rgb", "flags": ["alpha"], "components": [[0, 4, 1, 0, 8], [0, 4, 2, 0, 8], [0, 4, 3, 0, 8], [0, 4, 0, 0, 8]], "formats": "argb"},
    "rgba":         {"model": "rgb", "flags": ["alpha"], "components": [[0, 4, 0, 0, 8], [0, 4, 1, 0, 8], [0, 4, 2, 0, 8], [0, 4, 3, 0, 8]], "formats": "rgba"},
    "abgr":         {"model": "rgb", "flags": ["alpha"], "components": [[0, 4, 3, 0, 8], [0, 4, 2, 0, 8], [0, 4, 1, 0, 8], [0, 4, 0, 0, 8]], "formats": "abgr"},
    "bgra":         {"model": "rgb", "flags": ["alpha"], "components": [[0, 4, 2, 0, 8], [0, 4, 1, 0, 8], [0, 4, 0, 0, 8], [0, 4, 3, 0, 8]], "formats": "bgra"},
    "gray16":       {"model": "gray", "endians": ["le", "be"], "components": [[0, 2, 0, 0, 16]], "formats": "gray16", "fields": {"gray12": [0, 12], "gray10": [0, 10], "gray9": [0, 9], "gray14": [0, 14]}},
    "yuva420p":     {"model": "yuv", "flags": ["alpha"], "components": [[0, 1, 0, 0, 8], [1, 1, 0, 0, 8], [2, 1, 0, 0, 8], [3, 1, 0, 0, 8]], "formats": "yuva420p yuva422p yuva444p"},
    "rgb48":        {"model": "rgb", "endians": ["le", "be"], "components": [[0, 6, 0, 0, 16], [0, 6, 2, 0, 16], [0, 6, 4, 0, 16]], "formats": "rgb48"},
    "rgb565be":     {"model": "rgb", "flags": ["be"], "components": [[0, 2, -1, 3, 5], [0, 2, 0, 5, 6], [0, 2, 0, 0, 5]], "formats": "rgb565be"},
    "rgb565le":     {"model": "rgb", "components": [[0, 2, 1, 3, 5], [0, 2, 0, 5, 6], [0, 2, 0, 0, 5]], "formats": "rgb565le"},
    "rgb555be":     {"model": "rgb", "flags": ["be"], "components": [[0, 2, -1, 2, 5], [0, 2, 0, 5, 5], [0, 2, 0, 0, 5]], "formats": "rgb555be"},
    "rgb555le":     {"model": "rgb", "components": [[0, 2, 1, 2, 5], [0, 2, 0, 5, 5], [0, 2, 0, 0, 5]], "formats": "rgb555le"},
    "bgr565be":     {"model": "rgb", "flags": ["be"], "components": [[0, 2, 0, 0, 5], [0, 2, 0, 5, 6], [0, 2, -1, 3, 5]], "formats": "bgr565be"},
    "bgr565le":     {"model": "rgb", "components": [[0, 2, 0, 0, 5], [0, 2, 0, 5, 6], [0, 2, 1, 3, 5]], "formats": "bgr565le"},
    "bgr555be":     {"model": "rgb", "flags": ["be"], "components": [[0, 2, 0, 0, 5], [0, 2, 0, 5, 5], [0, 2, -1, 2, 5]], "formats": "bgr555be"},
    "bgr555le":     {"model": "rgb", "components": [[0, 2, 0, 0, 5], [0, 2, 0, 5, 5], [0, 2, 1, 2, 5]], "formats": "bgr555le"},
    "yuv420p16":    {"model": "yuv", "endians": ["le", "be"], "components": [[0, 2, 0, 0, 16], [1, 2, 0, 0, 16], [2, 2, 0, 0, 16]], "formats": "yuv420p16 yuv422p16 yuv444p16", "fields": {"yuv420p9 yuv444p9 yuv422p9": [0, 9], "yuv420p10 yuv422p10 yuv444p10 yuv440p10": [0, 10], "yuv420p12 yuv422p12 yuv444p12 yuv440p12": [0, 12], "yuv420p14 yuv422p14 yuv444p14": [0, 14], "yuv444p10msb": [6, 10], "yuv444p12msb": [4, 12]}},
    "rgb444le":     {"model": "rgb", "components": [[0, 2, 1, 0, 4], [0, 2, 0, 4, 4], [0, 2, 0, 0, 4]], "formats": "rgb444le"},
    "rgb444be":     {"model": "rgb", "flags": ["be"], "components": [[0, 2, -1, 0, 4], [0, 2, 0, 4, 4], [0, 2, 0, 0, 4]], "formats": "rgb444be"},
    "bgr444le":     {"model": "rgb", "components": [[0, 2, 0, 0, 4], [0, 2, 0, 4, 4], [0, 2, 1, 0, 4]], "formats": "bgr444le"},
    "bgr444be":     {"model": "rgb", "flags": ["be"], "components": [[0, 2, 0, 0, 4], [0, 2, 0, 4, 4], [0, 2, -1, 0, 4]], "formats": "bgr444be"},
    "ya8":          {"model": "gray", "flags": ["alpha"], "components": [[0, 2, 0, 0, 8], [0, 2, 1, 0, 8]], "formats": "ya8"},
    "bgr48":        {"model": "rgb", "endians": ["le", "be"], "components": [[0, 6, 4, 0, 16], [0, 6, 2, 0, 16], [0, 6, 0, 0, 16]], "formats": "bgr48"},
    "gbrp":         {"model": "rgb", "components": [[2, 1, 0, 0, 8], [0, 1, 0, 0, 8], [1, 1, 0, 0, 8]], "formats": "gbrp"},
    "gbrp16":       {"model": "rgb", "endians": ["le", "be"], "components": [[2, 2, 0, 0, 16], [0, 2, 0, 0, 16], [1, 2, 0, 0, 16]], "formats": "gbrp16", "fields": {"gbrp9": [0, 9], "gbrp10": [0, 10], "gbrp12": [0, 12], "gbrp14": [0, 14], "gbrp10msb": [6, 10], "gbrp12msb": [4, 12]}},
    "yuva420p16":   {"model": "yuv", "endians": ["le", "be"], "flags": ["alpha"], "components": [[0, 2, 0, 0, 16], [1, 2, 0, 0, 16], [2, 2, 0, 0, 16], [3, 2, 0, 0, 16]], "formats": "yuva420p16 yuva422p16 yuva444p16", "fields": {"yuva420p9 yuva422p9 yuva444p9": [0, 9], "yuva420p10 yuva422p10 yuva444p10": [0, 10], "yuva422p12 yuva444p12": [0, 12]}},
    "xyz12":        {"model": "xyz", "endians": ["le", "be"], "components": [[0, 6, 0, 4, 12], [0, 6, 2, 4, 12], [0, 6, 4, 4, 12]], "formats": "xyz12"},
    "p016":         {"model": "yuv", "endians": ["le", "be"], "components": [[0, 2, 0, 0, 16], [1, 4, 0, 0, 16], [1, 4, 2, 0, 16]], "formats": "p016 p216 p416", "fields": {"nv20": [0, 10], "p010 p210 p410": [6, 10], "p012 p212 p412": [4, 12]}},
    "rgba64":       {"model": "rgb", "endians": ["le", "be"], "flags": ["alpha"], "components": [[0, 8, 0, 0, 16], [0, 8, 2, 0, 16], [0, 8, 4, 0, 16], [0, 8, 6, 0, 16]], "formats": "rgba64"},
    "bgra64":       {"model": "rgb", "endians": ["le", "be"], "flags": ["alpha"], "components": [[0, 8, 4, 0, 16], [0, 8, 2, 0, 16], [0, 8, 0, 0, 16], [0, 8, 6, 0, 16]], "formats": "bgra64"},
    "yvyu422":      {"model": "yuv", "components": [[0, 2, 0, 0, 8], [0, 4, 3, 0, 8], [0, 4, 1, 0, 8]], "formats": "yvyu422"},
    "ya16":         {"model": "gray", "endians": ["le", "be"], "flags": ["alpha"], "components": [[0, 4, 0, 0, 16], [0, 4, 2, 0, 16]], "formats": "ya16"},
    "gbrap":        {"model": "rgb", "flags": ["alpha"], "components": [[2, 1, 0, 0, 8], [0, 1, 0, 0, 8], [1, 1, 0, 0, 8], [3, 1, 0, 0, 8]], "formats": "gbrap"},
    "gbrap16":      {"model": "rgb", "endians": ["le", "be"], "flags": ["alpha"], "components": [[2, 2, 0, 0, 16], [0, 2, 0, 0, 16], [1, 2, 0, 0, 16], [3, 2, 0, 0, 16]], "formats": "gbrap16", "fields": {"gbrap12": [0, 12], "gbrap10": [0, 10], "gbrap14": [0, 14]}},
    "0rgb":         {"model": "rgb", "components": [[0, 4, 1, 0, 8], [0, 4, 2, 0, 8], [0, 4, 3, 0, 8]], "formats": "0rgb"},
    "rgb0":         {"model": "rgb", "components": [[0, 4, 0, 0, 8], [0, 4, 1, 0, 8], [0, 4, 2, 0, 8]], "formats": "rgb0"},
    "0bgr":         {"model": "rgb", "components": [[0, 4, 3, 0, 8], [0, 4, 2, 0, 8], [0, 4, 1, 0, 8]], "formats": "0bgr"},
    "bgr0":         {"model": "rgb", "components": [[0, 4, 2, 0, 8], [0, 4, 1, 0, 8], [0, 4, 0, 0, 8]], "formats": "bgr0"},
    "bayer_bggr8":  {"model": "bayer", "components": [[0, 1, 0, 0, 2], [0, 1, 0, 0, 4], [0, 1, 0, 0, 2]], "formats": "bayer_bggr8 bayer_rggb8 bayer_gbrg8 bayer_grbg8"},
    "bayer_bggr16": {"model": "bayer", "endians": ["le", "be"], "components": [[0, 2, 0, 0, 4], [0, 2, 0, 0, 8], [0, 2, 0, 0, 4]], "formats": "bayer_bggr16 bayer_rggb16 bayer_gbrg16 bayer_grbg16"},
    "ayuv64":       {"model": "yuv", "endians": ["le", "be"], "flags": ["alpha"], "components": [[0, 8, 2, 0, 16], [0, 8, 4, 0, 16], [0, 8, 6, 0, 16], [0, 8, 0, 0, 16]], "formats": "ayuv64"},
    "gbrpf32":      {"model": "rgb", "endians": ["le", "be"], "flags": ["float"], "components": [[2, 4, 0, 0, 32], [0, 4, 0, 0, 32], [1, 4, 0, 0, 32]], "formats": "gbrpf32"},
    "gbrapf32":     {"model": "rgb", "endians": ["le", "be"], "flags": ["alpha", "float"], "components": [[2, 4, 0, 0, 32], [0, 4, 0, 0, 32], [1, 4, 0, 0, 32], [3, 4, 0, 0, 32]], "formats": "gbrapf32"},
    "grayf32":      {"model": "gray", "endians": ["le", "be"], "flags": ["float"], "components": [[0, 4, 0, 0, 32]], "formats": "grayf32"},
    "y216":         {"model": "yuv", "endians": ["le", "be"], "components": [[0, 4, 0, 0, 16], [0, 8, 2, 0, 16], [0, 8, 6, 0, 16]], "formats": "y216", "fields": {"y210": [6, 10], "y212": [4, 12]}},
    "x2rgb10le":    {"model": "rgb", "components": [[0, 4, 2, 4, 10], [0, 4, 1, 2, 10], [0, 4, 0, 0, 10]], "formats": "x2rgb10le"},
    "x2rgb10be":    {"model": "rgb", "flags": ["be"], "components": [[0, 4, 0, 4, 10], [0, 4, 1, 2, 10], [0, 4, 2, 0, 10]], "formats": "x2rgb10be"},
    "x2bgr10le":    {"model": "rgb", "components": [[0, 4, 0, 0, 10], [0, 4, 1, 2, 10], [0, 4, 2, 4, 10]], "formats": "x2bgr10le"},
    "x2bgr10be":    {"model": "rgb", "flags": ["be"], "components": [[0, 4, 2, 0, 10], [0, 4, 1, 2, 10], [0, 4, 0, 4, 10]], "formats": "x2bgr10be"},
    "vuya":         {"model": "yuv", "flags": ["alpha"], "components": [[0, 4, 2, 0, 8], [0, 4, 1, 0, 8], [0, 4, 0, 0, 8], [0, 4, 3, 0, 8]], "formats": "vuya"},
    "rgbaf16":      {"model": "rgb", "endians": ["le", "be"], "flags": ["alpha", "float"], "components": [[0, 8, 0, 0, 16], [0, 8, 2, 0, 16], [0, 8, 4, 0, 16], [0, 8, 6, 0, 16]], "formats": "rgbaf16"},
    "vuyx":         {"model": "yuv", "components": [[0, 4, 2, 0, 8], [0, 4, 1, 0, 8], [0, 4, 0, 0, 8]], "formats": "vuyx"},
    "xv30be":       {"model": "yuv", "flags": ["be", "bits"], "components": [[0, 32, 10, 0, 10], [0, 32, 0, 0, 10], [0, 32, 20, 0, 10]], "formats": "xv30be"},
    "xv30le":       {"model": "yuv", "components": [[0, 4, 1, 2, 10], [0, 4, 0, 0, 10], [0, 4, 2, 4, 10]], "formats": "xv30le"},
    "xv48":         {"model": "yuv", "endians": ["le", "be"], "components": [[0, 8, 2, 0, 16], [0, 8, 0, 0, 16], [0, 8, 4, 0, 16]], "formats": "xv48", "fields": {"xv36": [4, 12]}},
    "rgbf32":       {"model": "rgb", "endians": ["le", "be"], "flags": ["float"], "components": [[0, 12, 0, 0, 32], [0, 12, 4, 0, 32], [0, 12, 8, 0, 32]], "formats": "rgbf32"},
    "rgbaf32":      {"model": "rgb", "endians": ["le", "be"], "flags": ["alpha", "float"], "components": [[0, 16, 0, 0, 32], [0, 16, 4, 0, 32], [0, 16, 8, 0, 32], [0, 16, 12, 0, 32]], "formats": "rgbaf32"},
    "gray32":       {"model": "gray", "endians": ["le", "be"], "components": [[0, 4, 0, 0, 32]], "formats": "gray32"},
    "gbrap32":      {"model": "rgb", "endians": ["le", "be"], "flags": ["alpha"], "components": [[2, 4, 0, 0, 32], [0, 4, 0, 0, 32], [1, 4, 0, 0, 32], [3, 4, 0, 0, 32]], "formats": "gbrap32"},
    "vyu444":       {"model": "yuv", "components": [[0, 3, 1, 0, 8], [0, 3, 2, 0, 8], [0, 3, 0, 0, 8]], "formats": "vyu444"},
    "uyva":         {"model": "yuv", "flags": ["alpha"], "components": [[0, 4, 1, 0, 8], [0, 4, 0, 0, 8], [0, 4, 2, 0, 8], [0, 4, 3, 0, 8]], "formats": "uyva"},
    "ayuv":         {"model": "yuv", "flags": ["alpha"], "components": [[0, 4, 1, 0, 8], [0, 4, 2, 0, 8], [0, 4, 3, 0, 8], [0, 4, 0, 0, 8]], "formats": "ayuv"},
    "gbrpf16":      {"model": "rgb", "endians": ["le", "be"], "flags": ["float"], "components": [[2, 2, 0, 0, 16], [0, 2, 0, 0, 16], [1, 2, 0, 0, 16]], "formats": "gbrpf16"},
    "gbrapf16":     {"model": "rgb", "endians": ["le", "be"], "flags": ["alpha", "float"], "components": [[2, 2, 0, 0, 16], [0, 2, 0, 0, 16], [1, 2, 0, 0, 16], [3, 2, 0, 0, 16]], "formats": "gbrapf16"},
    "grayf16":      {"model": "gray", "endians": ["le", "be"], "flags": ["float"], "components": [[0, 2, 0, 0, 16]], "formats": "grayf16"},
    "yaf32":        {"model": "gray", "endians": ["le", "be"], "flags": ["alpha", "float"], "components": [[0, 8, 0, 0, 32], [0, 8, 4, 0, 32]], "formats": "yaf32"},
    "yaf16":        {"model": "gray", "endians": ["le", "be"], "flags": ["alpha", "float"], "components": [[0, 4, 0, 0, 16], [0, 4, 2, 0, 16]], "formats": "yaf16"},
    "rgbf16":       {"model": "rgb", "endians": ["le", "be"], "flags": ["float"], "components": [[0, 6, 0, 0, 16], [0, 6, 2, 0, 16], [0, 6, 4, 0, 16]], "formats": "rgbf16"},
    "v30xle":       {"model": "yuv", "components": [[0, 4, 1, 4, 10], [0, 4, 0, 2, 10], [0, 4, 2, 6, 10]], "formats": "v30xle"},
    "v30xbe":       {"model": "yuv", "flags": ["be", "bits"], "components": [[0, 32, 12, 0, 10], [0, 32, 2, 0, 10], [0, 32, 22, 0, 10]], "formats": "v30xbe"},
    "rgb96":        {"model": "rgb", "endians": ["le", "be"], "components": [[0, 12, 0, 0, 32], [0, 12, 4, 0, 32], [0, 12, 8, 0, 32]], "formats": "rgb96"},
    "rgba128":      {"model": "rgb", "endians": ["le", "be"], "flags": ["alpha"], "components": [[0, 16, 0, 0, 32], [0, 16, 4, 0, 32], [0, 16, 8, 0, 32], [0, 16, 12, 0, 32]], "formats": "rgba128"}
} %}
{%- set systems = {"yuv": ["linear", "cl", "ictcp"]} %}
{%- set shapes = {"ictcp": ["pq", "hlg"]} %}
{%- set chains = {
    "curve": [["same", "any"], ["convert", "sdr"], ["convert", "hdr"]],
    "identity": [["same", "any"]],
    "log": [["convert", "sdr"], ["convert", "hdr"]],
    "pq": [["convert", "sdr"], ["same", "hdr"], ["convert", "hdr"]],
    "hlg": [["convert", "sdr"], ["same", "hdr"], ["convert", "hdr"]],
} %}
{%- set matrices = {
    0: {"system": "linear", "toSignal": [[0, 0, 1], [1, 0, 0], [0, 1, 0]]},
    1: {"system": "linear", "weights": [0.2126, 0.0722]},
    2: {"system": "linear", "weights": "unspecified"},
    4: {"system": "linear", "weights": [0.30, 0.11]},
    5: {"system": "linear", "weights": [0.299, 0.114]},
    6: {"system": "linear", "weights": [0.299, 0.114]},
    7: {"system": "linear", "weights": [0.212, 0.087]},
    8: {"system": "linear", "toSignal": [[1, -1, 1], [1, 1, 0], [1, -1, -1]]},
    9: {"system": "linear", "weights": [0.2627, 0.0593]},
    10: {"system": "cl", "weights": [0.2627, 0.0593]},
    11: {"system": "linear", "toSignal": [[0.991902, 0, 2], [1, 0, 0], [1 / 0.986566, 2 / 0.986566, 0]]},
    12: {"system": "linear", "weights": "primaries"},
    13: {"system": "cl", "weights": "primaries"},
    14: {"system": "ictcp"},
    16: {"system": "linear", "toSignal": [[1, -0.5, 0.5], [1, 0.5, 0], [1, -0.5, -0.5]], "lumaBits": 2},
    17: {"system": "linear", "toSignal": [[1, -0.5, 0.5], [1, 0.5, 0], [1, -0.5, -0.5]], "lumaBits": 1},
} %}
{%- set power = {
    2.2: [[1, 2.2, 1, 0, 0], [1, 2.2, 1, 0, 0], -1],
    2.4: [[1, 2.4, 1, 0, 0], [1, 2.4, 1, 0, 0], -1],
    2.8: [[1, 2.8, 1, 0, 0], [1, 2.8, 1, 0, 0], -1],
} %}
{%- set bt709 = 1.099296826809442 %}
{%- set bt709Toe = 0.018053968510807 %}
{%- set curves = {
    "bt709": [[bt709, 0.45, 1, 0, bt709 - 1], [4.5, 1, 1, 0, 0], bt709Toe],
    "bt709Inverse": [[1, 1 / 0.45, 1 / bt709, (bt709 - 1) / bt709, 0], [1, 1, 1 / 4.5, 0, 0], 4.5 * bt709Toe],
    "smpte240": [[1.1115, 0.45, 1, 0, 0.1115], [4, 1, 1, 0, 0], 0.0228],
    "smpte240Inverse": [[1, 1 / 0.45, 1 / 1.1115, 0.1115 / 1.1115, 0], [1, 1, 0.25, 0, 0], 4 * 0.0228],
    "srgb": [[1.055, 1 / 2.4, 1, 0, 0.055], [12.92, 1, 1, 0, 0], 0.0031308],
    "srgbInverse": [[1, 2.4, 1 / 1.055, 0.055 / 1.055, 0], [1, 1, 1 / 12.92, 0, 0], 0.04045],
    "dci": [[1, 1 / 2.6, 48 / 52.37, 0, 0], [1, 1 / 2.6, 48 / 52.37, 0, 0], -1],
    "dciInverse": [[52.37 / 48, 2.6, 1, 0, 0], [52.37 / 48, 2.6, 1, 0, 0], -1],
    "linear": [[1, 1, 1, 0, 0], [1, 1, 1, 0, 0], -1],
    "root22": [[1, 1 / 2.2, 1, 0, 0], [1, 1 / 2.2, 1, 0, 0], -1],
    "root28": [[1, 1 / 2.8, 1, 0, 0], [1, 1 / 2.8, 1, 0, 0], -1],
} %}
{%- set transfers = {
    1: {"shape": "curve", "eotf": power[2.4], "oetf": curves.bt709, "inverse": curves.bt709Inverse},
    2: {"shape": "curve", "eotf": power[2.4], "oetf": curves.bt709, "inverse": curves.bt709Inverse},
    4: {"shape": "curve", "eotf": power[2.2], "oetf": curves.root22, "inverse": power[2.2]},
    5: {"shape": "curve", "eotf": power[2.8], "oetf": curves.root28, "inverse": power[2.8]},
    6: {"shape": "curve", "eotf": power[2.4], "oetf": curves.bt709, "inverse": curves.bt709Inverse},
    7: {"shape": "curve", "eotf": power[2.4], "oetf": curves.smpte240, "inverse": curves.smpte240Inverse},
    8: {"shape": "curve", "eotf": curves.linear, "oetf": curves.linear, "inverse": curves.linear},
    9: {"shape": "log", "decades": 2},
    10: {"shape": "log", "decades": 2.5},
    11: {"shape": "curve", "eotf": power[2.4], "oetf": curves.bt709, "inverse": curves.bt709Inverse},
    12: {"shape": "curve", "eotf": power[2.4], "oetf": curves.bt709, "inverse": curves.bt709Inverse},
    13: {"shape": "curve", "eotf": curves.srgbInverse, "oetf": curves.srgb, "inverse": curves.srgbInverse},
    14: {"shape": "curve", "eotf": power[2.4], "oetf": curves.bt709, "inverse": curves.bt709Inverse},
    15: {"shape": "curve", "eotf": power[2.4], "oetf": curves.bt709, "inverse": curves.bt709Inverse},
    16: {"shape": "pq"},
    17: {"shape": "curve", "eotf": curves.dciInverse, "oetf": curves.dci, "inverse": curves.dciInverse},
    18: {"shape": "hlg"},
} %}
{%- set primaries = {
    1: [0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290],
    2: [0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290],
    4: [0.670, 0.330, 0.210, 0.710, 0.140, 0.080, 0.3100, 0.3160],
    5: [0.640, 0.330, 0.290, 0.600, 0.150, 0.060, 0.3127, 0.3290],
    6: [0.630, 0.340, 0.310, 0.595, 0.155, 0.070, 0.3127, 0.3290],
    7: [0.630, 0.340, 0.310, 0.595, 0.155, 0.070, 0.3127, 0.3290],
    8: [0.681, 0.319, 0.243, 0.692, 0.145, 0.049, 0.3100, 0.3160],
    9: [0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290],
    10: "xyz",
    11: [0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3140, 0.3510],
    12: [0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290],
    22: [0.630, 0.340, 0.295, 0.605, 0.155, 0.077, 0.3127, 0.3290],
} %}
{%- set outputs = {"sdr": 1, "hdr": 9} %}
{%- set ranges = [0, 1, 2] %}
{%- set locations = {
    0: [0, 0.5],
    1: [0, 0.5],
    2: [0.5, 0.5],
    3: [0, 0],
    4: [0.5, 0],
    5: [0, 1],
    6: [0.5, 1],
} %}
{%- set components = source.components %}
{%- set bigEndian = "be" in source.flags %}
{%- set alpha = "alpha" in source.flags %}
{%- set yuv = source.model == "yuv" %}
{%- set shifts = {1: 2, 2: 1, 4: 0} %}
{%- set curved = transfer in ["curve", "identity"] %}

{%- macro hex(depth) -%}
0x{{ "%x" | format(2 ** depth - 1) }}u
{%- endmacro %}

{%- macro start(c) -%}
{%- set plane, step, offset, shift, depth = components[c] -%}
{{ offset + 1 if bigEndian and shift + depth <= 8 else offset }}
{%- endmacro %}

{%- macro value(c, window) -%}
{%- set plane, step, offset, shift, depth = components[c] -%}
{%- if "float" in source.flags and depth == 16 -%}
vec2(unpackHalf2x16({{ "swap16(" ~ window ~ ")" if bigEndian else window }}.x).x, unpackHalf2x16({{ "swap16(" ~ window ~ ")" if bigEndian else window }}.y).x)
{%- elif "float" in source.flags -%}
uintBitsToFloat({{ "swap32(" ~ window ~ ")" if bigEndian else window }})
{%- elif bigEndian and shift + depth > 16 -%}
vec2(swap32({{ window }}) >> {{ shift }}u & {{ hex(depth) }})
{%- elif bigEndian and shift + depth > 8 -%}
vec2(swap16({{ window }}) >> {{ shift }}u & {{ hex(depth) }})
{%- else -%}
vec2({{ window }}{% if shift %} >> {{ shift }}u{% endif %} & {{ hex(depth) }})
{%- endif -%}
{%- endmacro %}

{%- macro planeRows(plane) %}
    uint top{{ plane }} = (frame.planeOffset[{{ plane }}] + a.y * frame.lineSize[{{ plane }}]) >> 2;
    uint bottom{{ plane }} = (frame.planeOffset[{{ plane }}] + b.y * frame.lineSize[{{ plane }}]) >> 2;
{%- endmacro %}

{%- macro grid(name, members, extent) %}

vec4 {{ name }}(vec2 at) {
    vec2 base = floor(at);
    vec2 f = at - base;
    uvec2 a = uvec2(max(ivec2(base), 0));
    uvec2 b = uvec2(min(ivec2(base) + 1, ivec2({{ extent }}) - 1));
    uvec2 column = uvec2(a.x, b.x);
{%- set planes = namespace(done=[]) %}
{%- for c in members %}
{%- set plane, step, offset, shift, depth = components[c] %}
{%- if plane not in planes.done %}
{%- set planes.done = planes.done + [plane] %}
{{- planeRows(plane) }}
{%- if "bits" not in source.flags and not (c == 0 and "luma" in source) and step in shifts %}
{%- if step == 4 %}
    uvec2 above{{ plane }} = uvec2(words[top{{ plane }} + column.x], words[top{{ plane }} + column.y]);
    uvec2 below{{ plane }} = uvec2(words[bottom{{ plane }} + column.x], words[bottom{{ plane }} + column.y]);
{%- else %}
    uvec2 index{{ plane }} = column >> {{ shifts[step] }};
    uvec2 place{{ plane }} = (column & {{ 4 // step - 1 }}u) * {{ 8 * step }}u;
    uvec2 above{{ plane }} = uvec2(words[top{{ plane }} + index{{ plane }}.x], words[top{{ plane }} + index{{ plane }}.y]) >> place{{ plane }};
    uvec2 below{{ plane }} = uvec2(words[bottom{{ plane }} + index{{ plane }}.x], words[bottom{{ plane }} + index{{ plane }}.y]) >> place{{ plane }};
{%- endif %}
{%- elif "bits" not in source.flags and not (c == 0 and "luma" in source) and step % 4 == 0 %}
    uvec2 index{{ plane }} = column * {{ step // 4 }}u;
{%- set loaded = namespace(words=[]) %}
{%- for m in members %}
{%- set word = (start(m) | int) // 4 %}
{%- if components[m][0] == plane and word not in loaded.words %}
{%- set loaded.words = loaded.words + [word] %}
    uvec2 above{{ plane }}_{{ word }} = uvec2(words[top{{ plane }} + index{{ plane }}.x + {{ word }}u], words[top{{ plane }} + index{{ plane }}.y + {{ word }}u]);
    uvec2 below{{ plane }}_{{ word }} = uvec2(words[bottom{{ plane }} + index{{ plane }}.x + {{ word }}u], words[bottom{{ plane }} + index{{ plane }}.y + {{ word }}u]);
{%- endif %}
{%- endfor %}
{%- endif %}
{%- endif %}
{%- endfor %}
{%- for c in members %}
{%- set plane, step, offset, shift, depth = components[c] %}
{%- set first = (start(c) | int) %}
{%- if "bits" in source.flags and depth == 10 %}
    vec2 rows{{ c }} = mix(vec2(swap32(uvec2(words[top{{ plane }} + column.x], words[top{{ plane }} + column.y])) >> {{ offset }}u & {{ hex(depth) }}), vec2(swap32(uvec2(words[bottom{{ plane }} + column.x], words[bottom{{ plane }} + column.y])) >> {{ offset }}u & {{ hex(depth) }}), f.y);
{%- elif "bits" in source.flags %}
    uvec2 bit{{ c }} = column * {{ step }}u{% if offset %} + {{ offset }}u{% endif %};
    uvec2 byte{{ c }} = bit{{ c }} >> 3;
    uvec2 place{{ c }} = (byte{{ c }} & 3u) * 8u + {{ 8 - depth }}u - (bit{{ c }} & 7u);
    vec2 above{{ c }} = vec2(uvec2(words[top{{ plane }} + (byte{{ c }}.x >> 2)], words[top{{ plane }} + (byte{{ c }}.y >> 2)]) >> place{{ c }} & {{ hex(depth) }});
    vec2 below{{ c }} = vec2(uvec2(words[bottom{{ plane }} + (byte{{ c }}.x >> 2)], words[bottom{{ plane }} + (byte{{ c }}.y >> 2)]) >> place{{ c }} & {{ hex(depth) }});
    vec2 rows{{ c }} = mix(above{{ c }}, below{{ c }}, f.y);
{%- elif (c == 0 and "luma" in source) or step not in shifts and step % 4 != 0 %}
{%- if c == 0 and "luma" in source %}
    const uint group[4] = uint[]({{ source.luma[1] | join("u, ") }}u);
    uvec2 byte{{ c }} = (column >> 2) * {{ source.luma[0] }}u + uvec2(group[column.x & 3u], group[column.y & 3u]);
{%- else %}
    uvec2 byte{{ c }} = column * {{ step }}u{% if first %} + {{ first }}u{% endif %};
{%- endif %}
    uvec2 place{{ c }} = (byte{{ c }} & 3u) * 8u;
    uvec2 above{{ c }} = uvec2(words[top{{ plane }} + (byte{{ c }}.x >> 2)], words[top{{ plane }} + (byte{{ c }}.y >> 2)]) >> place{{ c }};
    uvec2 below{{ c }} = uvec2(words[bottom{{ plane }} + (byte{{ c }}.x >> 2)], words[bottom{{ plane }} + (byte{{ c }}.y >> 2)]) >> place{{ c }};
    vec2 rows{{ c }} = mix({{ value(c, "above" ~ c) }}, {{ value(c, "below" ~ c) }}, f.y);
{%- elif step in shifts %}
{%- set window = "above" ~ plane ~ (" >> " ~ 8 * first ~ "u" if first else "") %}
{%- set lower = "below" ~ plane ~ (" >> " ~ 8 * first ~ "u" if first else "") %}
    vec2 rows{{ c }} = mix({{ value(c, "(" ~ window ~ ")" if first else window) }}, {{ value(c, "(" ~ lower ~ ")" if first else lower) }}, f.y);
{%- else %}
{%- set word = first // 4 %}
{%- set within = first % 4 %}
{%- set window = "above" ~ plane ~ "_" ~ word ~ (" >> " ~ 8 * within ~ "u" if within else "") %}
{%- set lower = "below" ~ plane ~ "_" ~ word ~ (" >> " ~ 8 * within ~ "u" if within else "") %}
    vec2 rows{{ c }} = mix({{ value(c, "(" ~ window ~ ")" if within else window) }}, {{ value(c, "(" ~ lower ~ ")" if within else lower) }}, f.y);
{%- endif %}
{%- endfor %}
{%- set slots = namespace(values=["0.0", "0.0", "0.0", "0.0"]) %}
{%- for c in members %}
{%- set slot = 3 if (alpha and c == components | length - 1) else c %}
{%- set slots.values = slots.values[:slot] + ["mix(rows" ~ c ~ ".x, rows" ~ c ~ ".y, f.x)"] + slots.values[slot + 1:] %}
{%- endfor %}

    return vec4({{ slots.values | join(", ") }});
}
{%- endmacro %}

#version 450

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fColor;

layout(std430, set = 0, binding = 0) readonly buffer Bytes {
    uint words[];
};

layout(std140, set = 0, binding = 1) uniform Frame {
    uvec4 planeOffset;
    uvec4 lineSize;
    uvec4 size;
    vec4 chroma;
    mat3 decode;
    vec4 bias;
    vec4 sites;
    vec4 weights;
    vec4 curveUpper;
    vec4 curveLower;
    vec4 curveLimits;
    vec4 oetfUpper;
    vec4 oetfLower;
    vec4 oetfLimits;
    vec4 inverseUpper;
    vec4 inverseLower;
    vec4 inverseLimits;
    mat3 toOutput;
    vec4 light;
    vec4 luminance;
} frame;
{%- if bigEndian %}

uvec2 swap16(uvec2 value) {
    return (value & 0xffu) << 8 | value >> 8 & 0xffu;
}

uvec2 swap32(uvec2 value) {
    return (value & 0xffu) << 24 | (value & 0xff00u) << 8 | value >> 8 & 0xff00u | value >> 24;
}
{%- endif %}
{%- if source.model == "palette" %}

vec4 paletteColor(vec2 at) {
    vec2 base = floor(at);
    vec2 f = at - base;
    uvec2 a = uvec2(max(ivec2(base), 0));
    uvec2 b = uvec2(min(ivec2(base) + 1, ivec2(frame.size.xy) - 1));
    uvec2 column = uvec2(a.x, b.x);
{{- planeRows(0) }}
    uint palette = frame.planeOffset[1] >> 2;
    uvec2 index = column >> 2;
    uvec2 place = (column & 3u) * 8u;
    uvec2 above = uvec2(words[top0 + index.x], words[top0 + index.y]) >> place & 0xffu;
    uvec2 below = uvec2(words[bottom0 + index.x], words[bottom0 + index.y]) >> place & 0xffu;
    vec4 left = mix(unpackUnorm4x8(words[palette + above.x]), unpackUnorm4x8(words[palette + below.x]), f.y);
    vec4 right = mix(unpackUnorm4x8(words[palette + above.y]), unpackUnorm4x8(words[palette + below.y]), f.y);

    return mix(left, right, f.x).zyxw;
}
{%- elif source.model == "bayer" %}

float mosaic(ivec2 at) {
    ivec2 last = ivec2(frame.size.xy) - 1;
    uvec2 inside = uvec2(last - abs(last - abs(at)));
{%- if components[0][1] == 1 %}
    uint address = frame.planeOffset[0] + inside.y * frame.lineSize[0] + inside.x;

    return float(words[address >> 2] >> ((address & 3u) * 8u) & 0xffu);
{%- else %}
    uint address = frame.planeOffset[0] + inside.y * frame.lineSize[0] + inside.x * 2u;
    uvec2 window = uvec2(words[address >> 2] >> ((address & 2u) * 8u));

    return {{ "float(swap16(window).x)" if bigEndian else "float(window.x & 0xffffu)" }};
{%- endif %}
}

vec3 demosaic(ivec2 at) {
    ivec2 red = ivec2(frame.sites.xy);
    ivec2 phase = at & 1;
    float here = mosaic(at);
    float horizontal = (mosaic(at - ivec2(1, 0)) + mosaic(at + ivec2(1, 0))) / 2.0;
    float vertical = (mosaic(at - ivec2(0, 1)) + mosaic(at + ivec2(0, 1))) / 2.0;
    float cross = (horizontal + vertical) / 2.0;
    float diagonal = (mosaic(at + ivec2(-1, -1)) + mosaic(at + ivec2(1, -1)) + mosaic(at + ivec2(-1, 1)) + mosaic(at + ivec2(1, 1))) / 4.0;
    float onRed = float(all(equal(phase, red)));
    float onBlue = float(!any(equal(phase, red)));
    vec3 onGreen = mix(vec3(vertical, here, horizontal), vec3(horizontal, here, vertical), float(phase.y == red.y));

    return onRed * vec3(here, cross, diagonal) + onBlue * vec3(diagonal, cross, here) + (1.0 - onRed - onBlue) * onGreen;
}

vec4 bayerCodes(vec2 at) {
    vec2 base = floor(at);
    vec2 f = at - base;
    ivec2 a = max(ivec2(base), 0);
    ivec2 b = min(ivec2(base) + 1, ivec2(frame.size.xy) - 1);
    vec3 top = mix(demosaic(a), demosaic(ivec2(b.x, a.y)), f.x);
    vec3 bottom = mix(demosaic(ivec2(a.x, b.y)), demosaic(b), f.x);

    return vec4(mix(top, bottom, f.y), 0.0);
}
{%- elif yuv %}
{{- grid("lumaCodes", [0, 3] if alpha else [0], "frame.size.xy") }}
{{- grid("chromaCodes", [1, 2], "frame.size.zw") }}
{%- else %}
{{- grid("pixelCodes", range(components | length) | list, "frame.size.xy") }}
{%- endif %}
{%- if transfer == "curve" or system == "cl" and curved %}

vec3 piece(vec3 x, vec4 upper, vec4 lower, vec4 limits) {
    vec3 v = max(x, 0.0);
    bvec3 low = lessThanEqual(v, vec3(limits.z));
    vec3 scale = mix(vec3(upper.x), vec3(lower.x), low);
    vec3 power = mix(vec3(upper.y), vec3(lower.y), low);
    vec3 inputScale = mix(vec3(upper.z), vec3(lower.z), low);
    vec3 inputOffset = mix(vec3(upper.w), vec3(lower.w), low);
    vec3 offset = mix(vec3(limits.x), vec3(limits.y), low);

    return min(scale * exp2(power * log2(v * inputScale + inputOffset)) - offset, vec3(limits.w));
}
{%- endif %}
{%- if transfer == "pq" %}

vec3 pqLight(vec3 signal) {
    vec3 p = pow(clamp(signal, 0.0, 1.0), vec3(32.0 / 2523.0));

    return pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), vec3(16384.0 / 2610.0));
}
{%- elif transfer == "hlg" %}

vec3 hlgScene(vec3 signal) {
    vec3 v = clamp(signal, 0.0, 1.0);

    return mix(exp(v * 5.591816310 - 3.130917952) * (1.0 / 12.0) + 0.28466892 / 12.0, v * v * (1.0 / 3.0), lessThanEqual(v, vec3(0.5)));
}

vec3 hlgDisplay(vec3 scene) {
    return scene * pow(dot(scene, frame.luminance.xyz), 0.2) * frame.light.z;
}
{%- endif %}
{%- if system == "cl" %}

vec3 encodeLight(vec3 light) {
{%- if curved %}
    return piece(light, frame.oetfUpper, frame.oetfLower, frame.oetfLimits);
{%- elif transfer == "log" %}
    vec3 v = max(light, vec3(1e-30));

    return mix(1.0 + log2(v) * (0.30102999566 / frame.light.x), vec3(0.0), lessThan(v, vec3(exp2(-3.32192809489 * frame.light.x))));
{%- elif transfer == "pq" %}
    vec3 y = pow(clamp(light, 0.0, 1.0), vec3(2610.0 / 16384.0));

    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(2523.0 / 32.0));
{%- elif transfer == "hlg" %}
    vec3 v = max(light, 0.0);

    return mix(0.17883277 * log(max(12.0 * v - 0.28466892, 1e-6)) + 0.55991073, sqrt(3.0 * v), lessThanEqual(v, vec3(1.0 / 12.0)));
{%- endif %}
}

vec3 decodeLight(vec3 signal) {
{%- if curved %}
    return piece(signal, frame.inverseUpper, frame.inverseLower, frame.inverseLimits);
{%- elif transfer == "log" %}
    return mix(exp2((signal - 1.0) * (3.32192809489 * frame.light.x)), vec3(0.0), lessThanEqual(signal, vec3(0.0)));
{%- elif transfer == "pq" %}
    return pqLight(signal);
{%- elif transfer == "hlg" %}
    return hlgScene(signal);
{%- endif %}
}

vec3 constantLuminance(vec3 ycc) {
    vec2 k = frame.weights.xy;
    vec2 negative = encodeLight(vec3(1.0 - k.y, 1.0 - k.x, 0.0)).xy;
    vec2 positive = 1.0 - encodeLight(vec3(k.y, k.x, 0.0)).xy;
    vec2 chroma = 2.0 * ycc.yz * mix(positive, negative, lessThanEqual(ycc.yz, vec2(0.0)));
    vec3 yrb = vec3(ycc.x, ycc.x + chroma.y, ycc.x + chroma.x);
    vec3 light = decodeLight(yrb);
    float g = (light.x - k.x * light.y - k.y * light.z) / (1.0 - k.x - k.y);

    return vec3(yrb.y, encodeLight(vec3(g)).x, yrb.z);
}
{%- endif %}

void main() {
    vec2 at = vUv * vec2(frame.size.xy) - 0.5;
{%- if source.model == "palette" %}
    vec4 color = paletteColor(at);
    vec3 signal = color.rgb;
    float alpha = color.a;
{%- else %}
{%- if yuv %}
    vec4 codes = lumaCodes(at) + chromaCodes(at * frame.chroma.xy - frame.chroma.zw);
{%- elif source.model == "bayer" %}
    vec4 codes = bayerCodes(at);
{%- else %}
    vec4 codes = pixelCodes(at);
{%- endif %}
{%- if system == "cl" %}
    vec3 signal = constantLuminance(frame.decode * codes.xyz + frame.bias.xyz);
{%- else %}
    vec3 signal = frame.decode * codes.xyz + frame.bias.xyz;
{%- endif %}
    float alpha = {{ "codes.w * frame.bias.w" if alpha else "1.0" }};
{%- endif %}
{%- if transfer == "identity" %}

    fColor = vec4(clamp(signal, 0.0, frame.curveLimits.w), alpha);
{%- elif transfer == "curve" and conversion == "same" %}

    fColor = vec4(piece(signal, frame.curveUpper, frame.curveLower, frame.curveLimits), alpha);
{%- else %}
{%- if system == "ictcp" and transfer == "pq" %}
    vec3 light = {{ inverse_matrix([[1688, 2146, 262], [683, 2951, 462], [99, 309, 3688]], 4096) }} * pqLight({{ inverse_matrix([[2048, 2048, 0], [6610, -13613, 7003], [17933, -17390, -543]], 4096) }} * signal) * frame.light.y;
{%- elif system == "ictcp" and transfer == "hlg" %}
    vec3 light = hlgDisplay({{ inverse_matrix([[1688, 2146, 262], [683, 2951, 462], [99, 309, 3688]], 4096) }} * hlgScene({{ inverse_matrix([[2048, 2048, 0], [3625, -7465, 3840], [9500, -9212, -288]], 4096) }} * signal));
{%- elif transfer == "curve" %}
    vec3 light = piece(signal, frame.curveUpper, frame.curveLower, frame.curveLimits);
{%- elif transfer == "log" %}
    vec3 light = mix(exp2((signal - 1.0) * (3.32192809489 * frame.light.x)), vec3(0.0), lessThanEqual(signal, vec3(0.0)));
{%- elif transfer == "pq" %}
    vec3 light = pqLight(signal) * frame.light.y;
{%- elif transfer == "hlg" %}
    vec3 light = hlgDisplay(hlgScene(signal));
{%- endif %}
{%- set rgb = "frame.toOutput * light" if conversion == "convert" else "light" %}
{%- if output == "hdr" %}

    fColor = vec4({{ rgb }}, alpha);
{%- else %}
    vec3 display = clamp({{ rgb }}, 0.0, 1.0);

    fColor = vec4(mix(1.055 * pow(display, vec3(1.0 / 2.4)) - 0.055, display * 12.92, lessThanEqual(display, vec3(0.0031308))), alpha);
{%- endif %}
{%- endif %}
}
