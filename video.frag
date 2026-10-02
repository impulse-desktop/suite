{%- set layouts = {
    "yuv420p":        {"model": "yuv", "flags": [], "components": [[0, 1, 0, 0, 8], [1, 1, 0, 0, 8], [2, 1, 0, 0, 8]], "formats": ["yuv420p", "yuv422p", "yuv444p", "yuv410p", "yuv411p", "yuvj420p", "yuvj422p", "yuvj444p", "yuv440p", "yuvj440p", "yuvj411p"]},
    "yuyv422":        {"model": "yuv", "flags": [], "components": [[0, 2, 0, 0, 8], [0, 4, 1, 0, 8], [0, 4, 3, 0, 8]], "formats": ["yuyv422"]},
    "rgb24":          {"model": "rgb", "flags": [], "components": [[0, 3, 0, 0, 8], [0, 3, 1, 0, 8], [0, 3, 2, 0, 8]], "formats": ["rgb24"]},
    "bgr24":          {"model": "rgb", "flags": [], "components": [[0, 3, 2, 0, 8], [0, 3, 1, 0, 8], [0, 3, 0, 0, 8]], "formats": ["bgr24"]},
    "gray":           {"model": "gray", "flags": [], "components": [[0, 1, 0, 0, 8]], "formats": ["gray"]},
    "monow":          {"model": "gray", "flags": ["bits"], "components": [[0, 1, 0, 0, 1]], "inverted": true, "formats": ["monow"]},
    "monob":          {"model": "gray", "flags": ["bits"], "components": [[0, 1, 0, 7, 1]], "formats": ["monob"]},
    "pal8":           {"model": "palette", "flags": ["alpha"], "components": [[0, 1, 0, 0, 8]], "formats": ["pal8"]},
    "uyvy422":        {"model": "yuv", "flags": [], "components": [[0, 2, 1, 0, 8], [0, 4, 0, 0, 8], [0, 4, 2, 0, 8]], "formats": ["uyvy422"]},
    "uyyvyy411":      {"model": "yuv", "flags": [], "components": [[0, 4, 1, 0, 8], [0, 6, 0, 0, 8], [0, 6, 3, 0, 8]], "luma": [6, [1, 2, 4, 5]], "formats": ["uyyvyy411"]},
    "bgr8":           {"model": "rgb", "flags": [], "components": [[0, 1, 0, 0, 3], [0, 1, 0, 3, 3], [0, 1, 0, 6, 2]], "formats": ["bgr8"]},
    "bgr4":           {"model": "rgb", "flags": ["bits"], "components": [[0, 4, 3, 0, 1], [0, 4, 1, 0, 2], [0, 4, 0, 0, 1]], "formats": ["bgr4"]},
    "bgr4_byte":      {"model": "rgb", "flags": [], "components": [[0, 1, 0, 0, 1], [0, 1, 0, 1, 2], [0, 1, 0, 3, 1]], "formats": ["bgr4_byte"]},
    "rgb8":           {"model": "rgb", "flags": [], "components": [[0, 1, 0, 5, 3], [0, 1, 0, 2, 3], [0, 1, 0, 0, 2]], "formats": ["rgb8"]},
    "rgb4":           {"model": "rgb", "flags": ["bits"], "components": [[0, 4, 0, 0, 1], [0, 4, 1, 0, 2], [0, 4, 3, 0, 1]], "formats": ["rgb4"]},
    "rgb4_byte":      {"model": "rgb", "flags": [], "components": [[0, 1, 0, 3, 1], [0, 1, 0, 1, 2], [0, 1, 0, 0, 1]], "formats": ["rgb4_byte"]},
    "nv12":           {"model": "yuv", "flags": [], "components": [[0, 1, 0, 0, 8], [1, 2, 0, 0, 8], [1, 2, 1, 0, 8]], "formats": ["nv12", "nv16", "nv24"]},
    "nv21":           {"model": "yuv", "flags": [], "components": [[0, 1, 0, 0, 8], [1, 2, 1, 0, 8], [1, 2, 0, 0, 8]], "formats": ["nv21", "nv42"]},
    "argb":           {"model": "rgb", "flags": ["alpha"], "components": [[0, 4, 1, 0, 8], [0, 4, 2, 0, 8], [0, 4, 3, 0, 8], [0, 4, 0, 0, 8]], "formats": ["argb"]},
    "rgba":           {"model": "rgb", "flags": ["alpha"], "components": [[0, 4, 0, 0, 8], [0, 4, 1, 0, 8], [0, 4, 2, 0, 8], [0, 4, 3, 0, 8]], "formats": ["rgba"]},
    "abgr":           {"model": "rgb", "flags": ["alpha"], "components": [[0, 4, 3, 0, 8], [0, 4, 2, 0, 8], [0, 4, 1, 0, 8], [0, 4, 0, 0, 8]], "formats": ["abgr"]},
    "bgra":           {"model": "rgb", "flags": ["alpha"], "components": [[0, 4, 2, 0, 8], [0, 4, 1, 0, 8], [0, 4, 0, 0, 8], [0, 4, 3, 0, 8]], "formats": ["bgra"]},
    "gray16be":       {"model": "gray", "flags": ["be"], "components": [[0, 2, 0, 0, 16]], "formats": ["gray16be"]},
    "gray16le":       {"model": "gray", "flags": [], "components": [[0, 2, 0, 0, 16]], "formats": ["gray16le"]},
    "yuva420p":       {"model": "yuv", "flags": ["alpha"], "components": [[0, 1, 0, 0, 8], [1, 1, 0, 0, 8], [2, 1, 0, 0, 8], [3, 1, 0, 0, 8]], "formats": ["yuva420p", "yuva422p", "yuva444p"]},
    "rgb48be":        {"model": "rgb", "flags": ["be"], "components": [[0, 6, 0, 0, 16], [0, 6, 2, 0, 16], [0, 6, 4, 0, 16]], "formats": ["rgb48be"]},
    "rgb48le":        {"model": "rgb", "flags": [], "components": [[0, 6, 0, 0, 16], [0, 6, 2, 0, 16], [0, 6, 4, 0, 16]], "formats": ["rgb48le"]},
    "rgb565be":       {"model": "rgb", "flags": ["be"], "components": [[0, 2, -1, 3, 5], [0, 2, 0, 5, 6], [0, 2, 0, 0, 5]], "formats": ["rgb565be"]},
    "rgb565le":       {"model": "rgb", "flags": [], "components": [[0, 2, 1, 3, 5], [0, 2, 0, 5, 6], [0, 2, 0, 0, 5]], "formats": ["rgb565le"]},
    "rgb555be":       {"model": "rgb", "flags": ["be"], "components": [[0, 2, -1, 2, 5], [0, 2, 0, 5, 5], [0, 2, 0, 0, 5]], "formats": ["rgb555be"]},
    "rgb555le":       {"model": "rgb", "flags": [], "components": [[0, 2, 1, 2, 5], [0, 2, 0, 5, 5], [0, 2, 0, 0, 5]], "formats": ["rgb555le"]},
    "bgr565be":       {"model": "rgb", "flags": ["be"], "components": [[0, 2, 0, 0, 5], [0, 2, 0, 5, 6], [0, 2, -1, 3, 5]], "formats": ["bgr565be"]},
    "bgr565le":       {"model": "rgb", "flags": [], "components": [[0, 2, 0, 0, 5], [0, 2, 0, 5, 6], [0, 2, 1, 3, 5]], "formats": ["bgr565le"]},
    "bgr555be":       {"model": "rgb", "flags": ["be"], "components": [[0, 2, 0, 0, 5], [0, 2, 0, 5, 5], [0, 2, -1, 2, 5]], "formats": ["bgr555be"]},
    "bgr555le":       {"model": "rgb", "flags": [], "components": [[0, 2, 0, 0, 5], [0, 2, 0, 5, 5], [0, 2, 1, 2, 5]], "formats": ["bgr555le"]},
    "yuv420p16le":    {"model": "yuv", "flags": [], "components": [[0, 2, 0, 0, 16], [1, 2, 0, 0, 16], [2, 2, 0, 0, 16]], "formats": ["yuv420p16le", "yuv422p16le", "yuv444p16le"]},
    "yuv420p16be":    {"model": "yuv", "flags": ["be"], "components": [[0, 2, 0, 0, 16], [1, 2, 0, 0, 16], [2, 2, 0, 0, 16]], "formats": ["yuv420p16be", "yuv422p16be", "yuv444p16be"]},
    "rgb444le":       {"model": "rgb", "flags": [], "components": [[0, 2, 1, 0, 4], [0, 2, 0, 4, 4], [0, 2, 0, 0, 4]], "formats": ["rgb444le"]},
    "rgb444be":       {"model": "rgb", "flags": ["be"], "components": [[0, 2, -1, 0, 4], [0, 2, 0, 4, 4], [0, 2, 0, 0, 4]], "formats": ["rgb444be"]},
    "bgr444le":       {"model": "rgb", "flags": [], "components": [[0, 2, 0, 0, 4], [0, 2, 0, 4, 4], [0, 2, 1, 0, 4]], "formats": ["bgr444le"]},
    "bgr444be":       {"model": "rgb", "flags": ["be"], "components": [[0, 2, 0, 0, 4], [0, 2, 0, 4, 4], [0, 2, -1, 0, 4]], "formats": ["bgr444be"]},
    "ya8":            {"model": "gray", "flags": ["alpha"], "components": [[0, 2, 0, 0, 8], [0, 2, 1, 0, 8]], "formats": ["ya8"]},
    "bgr48be":        {"model": "rgb", "flags": ["be"], "components": [[0, 6, 4, 0, 16], [0, 6, 2, 0, 16], [0, 6, 0, 0, 16]], "formats": ["bgr48be"]},
    "bgr48le":        {"model": "rgb", "flags": [], "components": [[0, 6, 4, 0, 16], [0, 6, 2, 0, 16], [0, 6, 0, 0, 16]], "formats": ["bgr48le"]},
    "yuv420p9be":     {"model": "yuv", "flags": ["be"], "components": [[0, 2, 0, 0, 9], [1, 2, 0, 0, 9], [2, 2, 0, 0, 9]], "formats": ["yuv420p9be", "yuv444p9be", "yuv422p9be"]},
    "yuv420p9le":     {"model": "yuv", "flags": [], "components": [[0, 2, 0, 0, 9], [1, 2, 0, 0, 9], [2, 2, 0, 0, 9]], "formats": ["yuv420p9le", "yuv444p9le", "yuv422p9le"]},
    "yuv420p10be":    {"model": "yuv", "flags": ["be"], "components": [[0, 2, 0, 0, 10], [1, 2, 0, 0, 10], [2, 2, 0, 0, 10]], "formats": ["yuv420p10be", "yuv422p10be", "yuv444p10be", "yuv440p10be"]},
    "yuv420p10le":    {"model": "yuv", "flags": [], "components": [[0, 2, 0, 0, 10], [1, 2, 0, 0, 10], [2, 2, 0, 0, 10]], "formats": ["yuv420p10le", "yuv422p10le", "yuv444p10le", "yuv440p10le"]},
    "gbrp":           {"model": "rgb", "flags": [], "components": [[2, 1, 0, 0, 8], [0, 1, 0, 0, 8], [1, 1, 0, 0, 8]], "formats": ["gbrp"]},
    "gbrp9be":        {"model": "rgb", "flags": ["be"], "components": [[2, 2, 0, 0, 9], [0, 2, 0, 0, 9], [1, 2, 0, 0, 9]], "formats": ["gbrp9be"]},
    "gbrp9le":        {"model": "rgb", "flags": [], "components": [[2, 2, 0, 0, 9], [0, 2, 0, 0, 9], [1, 2, 0, 0, 9]], "formats": ["gbrp9le"]},
    "gbrp10be":       {"model": "rgb", "flags": ["be"], "components": [[2, 2, 0, 0, 10], [0, 2, 0, 0, 10], [1, 2, 0, 0, 10]], "formats": ["gbrp10be"]},
    "gbrp10le":       {"model": "rgb", "flags": [], "components": [[2, 2, 0, 0, 10], [0, 2, 0, 0, 10], [1, 2, 0, 0, 10]], "formats": ["gbrp10le"]},
    "gbrp16be":       {"model": "rgb", "flags": ["be"], "components": [[2, 2, 0, 0, 16], [0, 2, 0, 0, 16], [1, 2, 0, 0, 16]], "formats": ["gbrp16be"]},
    "gbrp16le":       {"model": "rgb", "flags": [], "components": [[2, 2, 0, 0, 16], [0, 2, 0, 0, 16], [1, 2, 0, 0, 16]], "formats": ["gbrp16le"]},
    "yuva420p9be":    {"model": "yuv", "flags": ["be", "alpha"], "components": [[0, 2, 0, 0, 9], [1, 2, 0, 0, 9], [2, 2, 0, 0, 9], [3, 2, 0, 0, 9]], "formats": ["yuva420p9be", "yuva422p9be", "yuva444p9be"]},
    "yuva420p9le":    {"model": "yuv", "flags": ["alpha"], "components": [[0, 2, 0, 0, 9], [1, 2, 0, 0, 9], [2, 2, 0, 0, 9], [3, 2, 0, 0, 9]], "formats": ["yuva420p9le", "yuva422p9le", "yuva444p9le"]},
    "yuva420p10be":   {"model": "yuv", "flags": ["be", "alpha"], "components": [[0, 2, 0, 0, 10], [1, 2, 0, 0, 10], [2, 2, 0, 0, 10], [3, 2, 0, 0, 10]], "formats": ["yuva420p10be", "yuva422p10be", "yuva444p10be"]},
    "yuva420p10le":   {"model": "yuv", "flags": ["alpha"], "components": [[0, 2, 0, 0, 10], [1, 2, 0, 0, 10], [2, 2, 0, 0, 10], [3, 2, 0, 0, 10]], "formats": ["yuva420p10le", "yuva422p10le", "yuva444p10le"]},
    "yuva420p16be":   {"model": "yuv", "flags": ["be", "alpha"], "components": [[0, 2, 0, 0, 16], [1, 2, 0, 0, 16], [2, 2, 0, 0, 16], [3, 2, 0, 0, 16]], "formats": ["yuva420p16be", "yuva422p16be", "yuva444p16be"]},
    "yuva420p16le":   {"model": "yuv", "flags": ["alpha"], "components": [[0, 2, 0, 0, 16], [1, 2, 0, 0, 16], [2, 2, 0, 0, 16], [3, 2, 0, 0, 16]], "formats": ["yuva420p16le", "yuva422p16le", "yuva444p16le"]},
    "xyz12le":        {"model": "xyz", "flags": [], "components": [[0, 6, 0, 4, 12], [0, 6, 2, 4, 12], [0, 6, 4, 4, 12]], "formats": ["xyz12le"]},
    "xyz12be":        {"model": "xyz", "flags": ["be"], "components": [[0, 6, 0, 4, 12], [0, 6, 2, 4, 12], [0, 6, 4, 4, 12]], "formats": ["xyz12be"]},
    "nv20le":         {"model": "yuv", "flags": [], "components": [[0, 2, 0, 0, 10], [1, 4, 0, 0, 10], [1, 4, 2, 0, 10]], "formats": ["nv20le"]},
    "nv20be":         {"model": "yuv", "flags": ["be"], "components": [[0, 2, 0, 0, 10], [1, 4, 0, 0, 10], [1, 4, 2, 0, 10]], "formats": ["nv20be"]},
    "rgba64be":       {"model": "rgb", "flags": ["be", "alpha"], "components": [[0, 8, 0, 0, 16], [0, 8, 2, 0, 16], [0, 8, 4, 0, 16], [0, 8, 6, 0, 16]], "formats": ["rgba64be"]},
    "rgba64le":       {"model": "rgb", "flags": ["alpha"], "components": [[0, 8, 0, 0, 16], [0, 8, 2, 0, 16], [0, 8, 4, 0, 16], [0, 8, 6, 0, 16]], "formats": ["rgba64le"]},
    "bgra64be":       {"model": "rgb", "flags": ["be", "alpha"], "components": [[0, 8, 4, 0, 16], [0, 8, 2, 0, 16], [0, 8, 0, 0, 16], [0, 8, 6, 0, 16]], "formats": ["bgra64be"]},
    "bgra64le":       {"model": "rgb", "flags": ["alpha"], "components": [[0, 8, 4, 0, 16], [0, 8, 2, 0, 16], [0, 8, 0, 0, 16], [0, 8, 6, 0, 16]], "formats": ["bgra64le"]},
    "yvyu422":        {"model": "yuv", "flags": [], "components": [[0, 2, 0, 0, 8], [0, 4, 3, 0, 8], [0, 4, 1, 0, 8]], "formats": ["yvyu422"]},
    "ya16be":         {"model": "gray", "flags": ["be", "alpha"], "components": [[0, 4, 0, 0, 16], [0, 4, 2, 0, 16]], "formats": ["ya16be"]},
    "ya16le":         {"model": "gray", "flags": ["alpha"], "components": [[0, 4, 0, 0, 16], [0, 4, 2, 0, 16]], "formats": ["ya16le"]},
    "gbrap":          {"model": "rgb", "flags": ["alpha"], "components": [[2, 1, 0, 0, 8], [0, 1, 0, 0, 8], [1, 1, 0, 0, 8], [3, 1, 0, 0, 8]], "formats": ["gbrap"]},
    "gbrap16be":      {"model": "rgb", "flags": ["be", "alpha"], "components": [[2, 2, 0, 0, 16], [0, 2, 0, 0, 16], [1, 2, 0, 0, 16], [3, 2, 0, 0, 16]], "formats": ["gbrap16be"]},
    "gbrap16le":      {"model": "rgb", "flags": ["alpha"], "components": [[2, 2, 0, 0, 16], [0, 2, 0, 0, 16], [1, 2, 0, 0, 16], [3, 2, 0, 0, 16]], "formats": ["gbrap16le"]},
    "0rgb":           {"model": "rgb", "flags": [], "components": [[0, 4, 1, 0, 8], [0, 4, 2, 0, 8], [0, 4, 3, 0, 8]], "formats": ["0rgb"]},
    "rgb0":           {"model": "rgb", "flags": [], "components": [[0, 4, 0, 0, 8], [0, 4, 1, 0, 8], [0, 4, 2, 0, 8]], "formats": ["rgb0"]},
    "0bgr":           {"model": "rgb", "flags": [], "components": [[0, 4, 3, 0, 8], [0, 4, 2, 0, 8], [0, 4, 1, 0, 8]], "formats": ["0bgr"]},
    "bgr0":           {"model": "rgb", "flags": [], "components": [[0, 4, 2, 0, 8], [0, 4, 1, 0, 8], [0, 4, 0, 0, 8]], "formats": ["bgr0"]},
    "yuv420p12be":    {"model": "yuv", "flags": ["be"], "components": [[0, 2, 0, 0, 12], [1, 2, 0, 0, 12], [2, 2, 0, 0, 12]], "formats": ["yuv420p12be", "yuv422p12be", "yuv444p12be", "yuv440p12be"]},
    "yuv420p12le":    {"model": "yuv", "flags": [], "components": [[0, 2, 0, 0, 12], [1, 2, 0, 0, 12], [2, 2, 0, 0, 12]], "formats": ["yuv420p12le", "yuv422p12le", "yuv444p12le", "yuv440p12le"]},
    "yuv420p14be":    {"model": "yuv", "flags": ["be"], "components": [[0, 2, 0, 0, 14], [1, 2, 0, 0, 14], [2, 2, 0, 0, 14]], "formats": ["yuv420p14be", "yuv422p14be", "yuv444p14be"]},
    "yuv420p14le":    {"model": "yuv", "flags": [], "components": [[0, 2, 0, 0, 14], [1, 2, 0, 0, 14], [2, 2, 0, 0, 14]], "formats": ["yuv420p14le", "yuv422p14le", "yuv444p14le"]},
    "gbrp12be":       {"model": "rgb", "flags": ["be"], "components": [[2, 2, 0, 0, 12], [0, 2, 0, 0, 12], [1, 2, 0, 0, 12]], "formats": ["gbrp12be"]},
    "gbrp12le":       {"model": "rgb", "flags": [], "components": [[2, 2, 0, 0, 12], [0, 2, 0, 0, 12], [1, 2, 0, 0, 12]], "formats": ["gbrp12le"]},
    "gbrp14be":       {"model": "rgb", "flags": ["be"], "components": [[2, 2, 0, 0, 14], [0, 2, 0, 0, 14], [1, 2, 0, 0, 14]], "formats": ["gbrp14be"]},
    "gbrp14le":       {"model": "rgb", "flags": [], "components": [[2, 2, 0, 0, 14], [0, 2, 0, 0, 14], [1, 2, 0, 0, 14]], "formats": ["gbrp14le"]},
    "bayer_bggr8":    {"model": "bayer", "flags": [], "components": [[0, 1, 0, 0, 2], [0, 1, 0, 0, 4], [0, 1, 0, 0, 2]], "formats": ["bayer_bggr8", "bayer_rggb8", "bayer_gbrg8", "bayer_grbg8"]},
    "bayer_bggr16le": {"model": "bayer", "flags": [], "components": [[0, 2, 0, 0, 4], [0, 2, 0, 0, 8], [0, 2, 0, 0, 4]], "formats": ["bayer_bggr16le", "bayer_rggb16le", "bayer_gbrg16le", "bayer_grbg16le"]},
    "bayer_bggr16be": {"model": "bayer", "flags": ["be"], "components": [[0, 2, 0, 0, 4], [0, 2, 0, 0, 8], [0, 2, 0, 0, 4]], "formats": ["bayer_bggr16be", "bayer_rggb16be", "bayer_gbrg16be", "bayer_grbg16be"]},
    "ayuv64le":       {"model": "yuv", "flags": ["alpha"], "components": [[0, 8, 2, 0, 16], [0, 8, 4, 0, 16], [0, 8, 6, 0, 16], [0, 8, 0, 0, 16]], "formats": ["ayuv64le"]},
    "ayuv64be":       {"model": "yuv", "flags": ["be", "alpha"], "components": [[0, 8, 2, 0, 16], [0, 8, 4, 0, 16], [0, 8, 6, 0, 16], [0, 8, 0, 0, 16]], "formats": ["ayuv64be"]},
    "p010le":         {"model": "yuv", "flags": [], "components": [[0, 2, 0, 6, 10], [1, 4, 0, 6, 10], [1, 4, 2, 6, 10]], "formats": ["p010le", "p210le", "p410le"]},
    "p010be":         {"model": "yuv", "flags": ["be"], "components": [[0, 2, 0, 6, 10], [1, 4, 0, 6, 10], [1, 4, 2, 6, 10]], "formats": ["p010be", "p210be", "p410be"]},
    "gbrap12be":      {"model": "rgb", "flags": ["be", "alpha"], "components": [[2, 2, 0, 0, 12], [0, 2, 0, 0, 12], [1, 2, 0, 0, 12], [3, 2, 0, 0, 12]], "formats": ["gbrap12be"]},
    "gbrap12le":      {"model": "rgb", "flags": ["alpha"], "components": [[2, 2, 0, 0, 12], [0, 2, 0, 0, 12], [1, 2, 0, 0, 12], [3, 2, 0, 0, 12]], "formats": ["gbrap12le"]},
    "gbrap10be":      {"model": "rgb", "flags": ["be", "alpha"], "components": [[2, 2, 0, 0, 10], [0, 2, 0, 0, 10], [1, 2, 0, 0, 10], [3, 2, 0, 0, 10]], "formats": ["gbrap10be"]},
    "gbrap10le":      {"model": "rgb", "flags": ["alpha"], "components": [[2, 2, 0, 0, 10], [0, 2, 0, 0, 10], [1, 2, 0, 0, 10], [3, 2, 0, 0, 10]], "formats": ["gbrap10le"]},
    "gray12be":       {"model": "gray", "flags": ["be"], "components": [[0, 2, 0, 0, 12]], "formats": ["gray12be"]},
    "gray12le":       {"model": "gray", "flags": [], "components": [[0, 2, 0, 0, 12]], "formats": ["gray12le"]},
    "gray10be":       {"model": "gray", "flags": ["be"], "components": [[0, 2, 0, 0, 10]], "formats": ["gray10be"]},
    "gray10le":       {"model": "gray", "flags": [], "components": [[0, 2, 0, 0, 10]], "formats": ["gray10le"]},
    "p016le":         {"model": "yuv", "flags": [], "components": [[0, 2, 0, 0, 16], [1, 4, 0, 0, 16], [1, 4, 2, 0, 16]], "formats": ["p016le", "p216le", "p416le"]},
    "p016be":         {"model": "yuv", "flags": ["be"], "components": [[0, 2, 0, 0, 16], [1, 4, 0, 0, 16], [1, 4, 2, 0, 16]], "formats": ["p016be", "p216be", "p416be"]},
    "gray9be":        {"model": "gray", "flags": ["be"], "components": [[0, 2, 0, 0, 9]], "formats": ["gray9be"]},
    "gray9le":        {"model": "gray", "flags": [], "components": [[0, 2, 0, 0, 9]], "formats": ["gray9le"]},
    "gbrpf32be":      {"model": "rgb", "flags": ["be", "float"], "components": [[2, 4, 0, 0, 32], [0, 4, 0, 0, 32], [1, 4, 0, 0, 32]], "formats": ["gbrpf32be"]},
    "gbrpf32le":      {"model": "rgb", "flags": ["float"], "components": [[2, 4, 0, 0, 32], [0, 4, 0, 0, 32], [1, 4, 0, 0, 32]], "formats": ["gbrpf32le"]},
    "gbrapf32be":     {"model": "rgb", "flags": ["be", "alpha", "float"], "components": [[2, 4, 0, 0, 32], [0, 4, 0, 0, 32], [1, 4, 0, 0, 32], [3, 4, 0, 0, 32]], "formats": ["gbrapf32be"]},
    "gbrapf32le":     {"model": "rgb", "flags": ["alpha", "float"], "components": [[2, 4, 0, 0, 32], [0, 4, 0, 0, 32], [1, 4, 0, 0, 32], [3, 4, 0, 0, 32]], "formats": ["gbrapf32le"]},
    "gray14be":       {"model": "gray", "flags": ["be"], "components": [[0, 2, 0, 0, 14]], "formats": ["gray14be"]},
    "gray14le":       {"model": "gray", "flags": [], "components": [[0, 2, 0, 0, 14]], "formats": ["gray14le"]},
    "grayf32be":      {"model": "gray", "flags": ["be", "float"], "components": [[0, 4, 0, 0, 32]], "formats": ["grayf32be"]},
    "grayf32le":      {"model": "gray", "flags": ["float"], "components": [[0, 4, 0, 0, 32]], "formats": ["grayf32le"]},
    "yuva422p12be":   {"model": "yuv", "flags": ["be", "alpha"], "components": [[0, 2, 0, 0, 12], [1, 2, 0, 0, 12], [2, 2, 0, 0, 12], [3, 2, 0, 0, 12]], "formats": ["yuva422p12be", "yuva444p12be"]},
    "yuva422p12le":   {"model": "yuv", "flags": ["alpha"], "components": [[0, 2, 0, 0, 12], [1, 2, 0, 0, 12], [2, 2, 0, 0, 12], [3, 2, 0, 0, 12]], "formats": ["yuva422p12le", "yuva444p12le"]},
    "y210be":         {"model": "yuv", "flags": ["be"], "components": [[0, 4, 0, 6, 10], [0, 8, 2, 6, 10], [0, 8, 6, 6, 10]], "formats": ["y210be"]},
    "y210le":         {"model": "yuv", "flags": [], "components": [[0, 4, 0, 6, 10], [0, 8, 2, 6, 10], [0, 8, 6, 6, 10]], "formats": ["y210le"]},
    "x2rgb10le":      {"model": "rgb", "flags": [], "components": [[0, 4, 2, 4, 10], [0, 4, 1, 2, 10], [0, 4, 0, 0, 10]], "formats": ["x2rgb10le"]},
    "x2rgb10be":      {"model": "rgb", "flags": ["be"], "components": [[0, 4, 0, 4, 10], [0, 4, 1, 2, 10], [0, 4, 2, 0, 10]], "formats": ["x2rgb10be"]},
    "x2bgr10le":      {"model": "rgb", "flags": [], "components": [[0, 4, 0, 0, 10], [0, 4, 1, 2, 10], [0, 4, 2, 4, 10]], "formats": ["x2bgr10le"]},
    "x2bgr10be":      {"model": "rgb", "flags": ["be"], "components": [[0, 4, 2, 0, 10], [0, 4, 1, 2, 10], [0, 4, 0, 4, 10]], "formats": ["x2bgr10be"]},
    "vuya":           {"model": "yuv", "flags": ["alpha"], "components": [[0, 4, 2, 0, 8], [0, 4, 1, 0, 8], [0, 4, 0, 0, 8], [0, 4, 3, 0, 8]], "formats": ["vuya"]},
    "rgbaf16be":      {"model": "rgb", "flags": ["be", "alpha", "float"], "components": [[0, 8, 0, 0, 16], [0, 8, 2, 0, 16], [0, 8, 4, 0, 16], [0, 8, 6, 0, 16]], "formats": ["rgbaf16be"]},
    "rgbaf16le":      {"model": "rgb", "flags": ["alpha", "float"], "components": [[0, 8, 0, 0, 16], [0, 8, 2, 0, 16], [0, 8, 4, 0, 16], [0, 8, 6, 0, 16]], "formats": ["rgbaf16le"]},
    "vuyx":           {"model": "yuv", "flags": [], "components": [[0, 4, 2, 0, 8], [0, 4, 1, 0, 8], [0, 4, 0, 0, 8]], "formats": ["vuyx"]},
    "p012le":         {"model": "yuv", "flags": [], "components": [[0, 2, 0, 4, 12], [1, 4, 0, 4, 12], [1, 4, 2, 4, 12]], "formats": ["p012le", "p212le", "p412le"]},
    "p012be":         {"model": "yuv", "flags": ["be"], "components": [[0, 2, 0, 4, 12], [1, 4, 0, 4, 12], [1, 4, 2, 4, 12]], "formats": ["p012be", "p212be", "p412be"]},
    "y212be":         {"model": "yuv", "flags": ["be"], "components": [[0, 4, 0, 4, 12], [0, 8, 2, 4, 12], [0, 8, 6, 4, 12]], "formats": ["y212be"]},
    "y212le":         {"model": "yuv", "flags": [], "components": [[0, 4, 0, 4, 12], [0, 8, 2, 4, 12], [0, 8, 6, 4, 12]], "formats": ["y212le"]},
    "xv30be":         {"model": "yuv", "flags": ["be", "bits"], "components": [[0, 32, 10, 0, 10], [0, 32, 0, 0, 10], [0, 32, 20, 0, 10]], "formats": ["xv30be"]},
    "xv30le":         {"model": "yuv", "flags": [], "components": [[0, 4, 1, 2, 10], [0, 4, 0, 0, 10], [0, 4, 2, 4, 10]], "formats": ["xv30le"]},
    "xv36be":         {"model": "yuv", "flags": ["be"], "components": [[0, 8, 2, 4, 12], [0, 8, 0, 4, 12], [0, 8, 4, 4, 12]], "formats": ["xv36be"]},
    "xv36le":         {"model": "yuv", "flags": [], "components": [[0, 8, 2, 4, 12], [0, 8, 0, 4, 12], [0, 8, 4, 4, 12]], "formats": ["xv36le"]},
    "rgbf32be":       {"model": "rgb", "flags": ["be", "float"], "components": [[0, 12, 0, 0, 32], [0, 12, 4, 0, 32], [0, 12, 8, 0, 32]], "formats": ["rgbf32be"]},
    "rgbf32le":       {"model": "rgb", "flags": ["float"], "components": [[0, 12, 0, 0, 32], [0, 12, 4, 0, 32], [0, 12, 8, 0, 32]], "formats": ["rgbf32le"]},
    "rgbaf32be":      {"model": "rgb", "flags": ["be", "alpha", "float"], "components": [[0, 16, 0, 0, 32], [0, 16, 4, 0, 32], [0, 16, 8, 0, 32], [0, 16, 12, 0, 32]], "formats": ["rgbaf32be"]},
    "rgbaf32le":      {"model": "rgb", "flags": ["alpha", "float"], "components": [[0, 16, 0, 0, 32], [0, 16, 4, 0, 32], [0, 16, 8, 0, 32], [0, 16, 12, 0, 32]], "formats": ["rgbaf32le"]},
    "gbrap14be":      {"model": "rgb", "flags": ["be", "alpha"], "components": [[2, 2, 0, 0, 14], [0, 2, 0, 0, 14], [1, 2, 0, 0, 14], [3, 2, 0, 0, 14]], "formats": ["gbrap14be"]},
    "gbrap14le":      {"model": "rgb", "flags": ["alpha"], "components": [[2, 2, 0, 0, 14], [0, 2, 0, 0, 14], [1, 2, 0, 0, 14], [3, 2, 0, 0, 14]], "formats": ["gbrap14le"]}
} %}
{%- set models = {
    "yuv": ["linear", "cl", "ictcp"],
    "rgb": ["rgb"],
    "gray": ["gray"],
    "xyz": ["xyz"],
    "palette": ["palette"],
    "bayer": ["bayer"],
} %}
{%- set matrices = {
    "linear": [0, 1, 2, 4, 5, 6, 7, 8, 9, 11, 12, 16, 17],
    "cl": [10, 13],
    "ictcp": [14],
} %}
{%- set transfers = {
    "power": [1, 2, 4, 5, 6, 7, 11, 12, 14, 15, 17],
    "srgb": [13],
    "linear": [8],
    "log": [9, 10],
    "pq": [16],
    "hlg": [18],
} %}
{%- set signals = {
    "ictcp": ["pq", "hlg"],
} %}
{%- set outputs = ["hdr", "sdr"] %}
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
{%- set ranges = [0, 1, 2] %}
{%- set locations = [0, 1, 2, 3, 4, 5, 6] %}
{%- set source = layouts[layout] %}
{%- set components = source.components %}
{%- set alpha = "alpha" in source.flags %}

#version 450

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fColor;

layout(std430, set = 0, binding = 0) readonly buffer Bytes {
    uint words[];
};

layout(std140, set = 0, binding = 1) uniform Frame {
    uvec4 planeWord;
    uvec4 lineWords;
    uvec4 size;
    uvec4 tags;
    uvec4 sites;
    vec4 white;
} frame;

const bool bigEndian = {{ "true" if "be" in source.flags else "false" }};

uvec2 lumaSize() {
    return frame.size.xy;
}

uvec2 chromaShift() {
    return frame.size.zw;
}

uvec2 chromaSize() {
    return (lumaSize() + (uvec2(1u) << chromaShift()) - 1u) >> chromaShift();
}

uint matrixCode() {
    return frame.tags.x;
}

uint rangeCode() {
    return frame.tags.y;
}

uint transferCode() {
    return frame.tags.z;
}

uint primariesCode() {
    return frame.tags.w;
}

uint wordAt(uint plane, uint y, uint word) {
    return words[frame.planeWord[plane] + y * frame.lineWords[plane] + word];
}

uint mask(uint bits) {
    return bits >= 32u ? 0xffffffffu : (1u << bits) - 1u;
}

uint swapped(uint value, uint bytes) {
    uint reversed = ((value & 0xffu) << 24) | ((value & 0xff00u) << 8) | ((value >> 8) & 0xff00u) | (value >> 24);

    return reversed >> (32u - 8u * bytes);
}

uvec2 placeOf(uint x, uint step, uint start) {
    if (step % 4u == 0u) {
        return uvec2(x * (step / 4u) + start / 4u, start % 4u * 8u);
    }

    if (4u % step == 0u) {
        return uvec2(x / (4u / step), (x % (4u / step) * step + start) * 8u);
    }

    uint byte = x * step + start;

    return uvec2(byte / 4u, byte % 4u * 8u);
}

uvec2 byteAt(uint byte) {
    return uvec2(byte / 4u, byte % 4u * 8u);
}

uint windowAt(uint plane, uint y, uvec2 place, uint bytes) {
    uint value = (wordAt(plane, y, place.x) >> place.y) & mask(8u * bytes);

    return bigEndian && bytes > 1u ? swapped(value, bytes) : value;
}

uint field(uint plane, uvec2 at, uint step, int offset, uint shift, uint depth) {
    uint bytes = shift + depth <= 8u ? 1u : shift + depth <= 16u ? 2u : 4u;
    uint start = uint(bigEndian && bytes == 1u ? offset + 1 : offset);

    return (windowAt(plane, at.y, placeOf(at.x, step, start), bytes) >> shift) & mask(depth);
}

uint bitField(uint plane, uvec2 at, uint step, uint offset, uint depth) {
    if (depth == 10u) {
        return (swapped(wordAt(plane, at.y, at.x), 4u) >> offset) & mask(depth);
    }

    uint bit = at.x * step + offset;
    uint byte = bit / 8u;
    uint value = (wordAt(plane, at.y, byte / 4u) >> (byte % 4u * 8u)) & 0xffu;

    return (value >> (8u - depth - bit % 8u)) & mask(depth);
}

float floatField(uint plane, uvec2 at, uint step, uint offset, uint depth) {
    uint value = windowAt(plane, at.y, placeOf(at.x, step, offset), depth / 8u);

    return depth == 16u ? unpackHalf2x16(value).x : uintBitsToFloat(value);
}

vec2 taps(vec2 at, uvec2 extent, out uvec2 a, out uvec2 b) {
    vec2 base = floor(at);
    ivec2 last = ivec2(extent) - 1;

    a = uvec2(clamp(ivec2(base), ivec2(0), last));
    b = uvec2(clamp(ivec2(base) + 1, ivec2(0), last));

    return at - base;
}
{%- if source.model == "bayer" %}

float code(uvec2 at) {
    return float(field(0u, at, {{ components[0][1] }}u, 0, 0u, {{ 8 * components[0][1] }}u));
}
{%- elif source.model == "palette" %}

float code(uvec2 at) {
    return float(field(0u, at, 1u, 0, 0u, 8u));
}
{%- else %}
{%- for plane, step, offset, shift, depth in components %}

float code{{ loop.index0 }}(uvec2 at) {
{%- if "float" in source.flags %}
    return floatField({{ plane }}u, at, {{ step }}u, {{ offset }}u, {{ depth }}u);
{%- elif "bits" in source.flags and "inverted" in source %}
    return float(mask({{ depth }}u) - bitField({{ plane }}u, at, {{ step }}u, {{ offset }}u, {{ depth }}u));
{%- elif "bits" in source.flags %}
    return float(bitField({{ plane }}u, at, {{ step }}u, {{ offset }}u, {{ depth }}u));
{%- elif loop.first and "luma" in source %}
    const uint group[4] = uint[]({{ source.luma[1] | join("u, ") }}u);

    return float(windowAt({{ plane }}u, at.y, byteAt(at.x / 4u * {{ source.luma[0] }}u + group[at.x % 4u]), 1u));
{%- else %}
    return float(field({{ plane }}u, at, {{ step }}u, {{ offset }}, {{ shift }}u, {{ depth }}u));
{%- endif %}
}

float sample{{ loop.index0 }}(vec2 at, uvec2 extent) {
    uvec2 a;
    uvec2 b;
    vec2 f = taps(at, extent, a, b);
    float top = mix(code{{ loop.index0 }}(a), code{{ loop.index0 }}(uvec2(b.x, a.y)), f.x);
    float bottom = mix(code{{ loop.index0 }}(uvec2(a.x, b.y)), code{{ loop.index0 }}(b), f.x);

    return mix(top, bottom, f.y);
}
{%- endfor %}
{%- endif %}

mat3 primariesToXyz() {
{%- if source.model == "xyz" %}
    return mat3(1.0);
{%- else %}
    switch (primariesCode()) {
{%- for code, chromaticities in primaries.items() %}
        case {{ code }}u:
            return {{ rgb_to_xyz(chromaticities) }};
{%- endfor %}
        default:
            return {{ rgb_to_xyz(primaries[1]) }};
    }
{%- endif %}
}

vec3 luminanceWeights() {
    mat3 toXyz = primariesToXyz();

    return vec3(toXyz[0].y, toXyz[1].y, toXyz[2].y);
}

bool fullRange(uint depth) {
{%- if source.model == "yuv" %}
    return depth < 8u || rangeCode() == 2u;
{%- else %}
    return depth < 8u || rangeCode() != 1u;
{%- endif %}
}

vec2 lumaLevels(uint depth) {
{%- if "float" in source.flags %}
    return vec2(0.0, 1.0);
{%- else %}
{%- if color == "linear" %}
    if (matrixCode() == 16u || matrixCode() == 17u) {
        return vec2(0.0, exp2(float(depth - (matrixCode() == 16u ? 2u : 1u))) - 1.0);
    }

{%- endif %}
    float unit = exp2(float(depth) - 8.0);

    return fullRange(depth) ? vec2(0.0, exp2(float(depth)) - 1.0) : vec2(16.0, 219.0) * unit;
{%- endif %}
}

vec2 chromaLevels(uint depth) {
{%- if "float" in source.flags %}
    return vec2(0.0, 1.0);
{%- else %}
{%- if color == "linear" %}
    if (matrixCode() == 0u) {
        return lumaLevels(depth);
    }

    if (matrixCode() == 16u || matrixCode() == 17u) {
        return vec2(exp2(float(depth) - 1.0), lumaLevels(depth).y);
    }

{%- endif %}
    float unit = exp2(float(depth) - 8.0);

    return fullRange(depth) ? vec2(exp2(float(depth) - 1.0), exp2(float(depth)) - 1.0) : vec2(128.0, 224.0) * unit;
{%- endif %}
}

vec2 alphaLevels(uint depth) {
{%- if "float" in source.flags %}
    return vec2(0.0, 1.0);
{%- else %}
    return vec2(0.0, exp2(float(depth)) - 1.0);
{%- endif %}
}

float decode(float code, vec2 levels) {
    return (code - levels.x) / levels.y;
}

vec3 bt709Encode(vec3 light, float alpha, float beta, float slope) {
    vec3 v = abs(light);

    return sign(light) * mix(alpha * pow(v, vec3(0.45)) - (alpha - 1.0), slope * v, lessThan(v, vec3(beta)));
}

vec3 bt709Decode(vec3 signal, float alpha, float beta, float slope) {
    vec3 v = abs(signal);

    return sign(signal) * mix(pow((v + alpha - 1.0) / alpha, vec3(1.0 / 0.45)), v / slope, lessThan(v, vec3(slope * beta)));
}

vec3 pqNits(vec3 signal) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    vec3 p = pow(clamp(signal, 0.0, 1.0), vec3(1.0 / m2));

    return pow(max(p - c1, 0.0) / (c2 - c3 * p), vec3(1.0 / m1)) * 10000.0;
}

vec3 pqSignal(vec3 nits) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));

    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

vec3 hlgScene(vec3 signal) {
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    vec3 v = clamp(signal, 0.0, 1.0);

    return mix((exp((v - c) / a) + b) / 12.0, v * v / 3.0, lessThanEqual(v, vec3(0.5)));
}

vec3 hlgSignal(vec3 scene) {
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    vec3 v = max(scene, 0.0);

    return mix(a * log(max(12.0 * v - b, 1e-6)) + c, sqrt(3.0 * v), lessThanEqual(v, vec3(1.0 / 12.0)));
}

vec3 hlgNits(vec3 scene) {
    const float peak = 1000.0;
    const float systemGamma = 1.2;
    float luminance = max(dot(scene, luminanceWeights()), 0.0);

    return peak * pow(luminance, systemGamma - 1.0) * scene;
}

vec3 eotf(vec3 signal) {
{%- if transfer == "power" %}
    float exponent = transferCode() == 4u ? 2.2 : transferCode() == 5u ? 2.8 : transferCode() == 17u ? 2.6 : 2.4;
    float scale = transferCode() == 17u ? 52.37 / 48.0 : 1.0;

    return pow(max(signal, vec3(0.0)), vec3(exponent)) * scale;
{%- elif transfer == "srgb" %}
    vec3 v = max(signal, vec3(0.0));

    return mix(pow((v + 0.055) / 1.055, vec3(2.4)), v / 12.92, lessThanEqual(v, vec3(0.04045)));
{%- elif transfer == "linear" %}
    return signal;
{%- elif transfer == "log" %}
    float decades = transferCode() == 9u ? 2.0 : 2.5;

    return mix(pow(vec3(10.0), (signal - 1.0) * decades), vec3(0.0), lessThanEqual(signal, vec3(0.0)));
{%- elif transfer == "pq" %}
    return pqNits(signal) / frame.white.x;
{%- elif transfer == "hlg" %}
    return hlgNits(hlgScene(signal)) / frame.white.x;
{%- endif %}
}
{%- if color == "cl" %}

vec3 oetf(vec3 light) {
{%- if transfer == "power" %}
    switch (transferCode()) {
        case 4u:
            return pow(max(light, vec3(0.0)), vec3(1.0 / 2.2));
        case 5u:
            return pow(max(light, vec3(0.0)), vec3(1.0 / 2.8));
        case 7u:
            return bt709Encode(light, 1.1115, 0.0228, 4.0);
        case 17u:
            return pow(max(light * 48.0 / 52.37, vec3(0.0)), vec3(1.0 / 2.6));
        default:
            return bt709Encode(light, 1.099296826809442, 0.018053968510807, 4.5);
    }
{%- elif transfer == "srgb" %}
    vec3 v = max(light, vec3(0.0));

    return mix(1.055 * pow(v, vec3(1.0 / 2.4)) - 0.055, v * 12.92, lessThanEqual(v, vec3(0.0031308)));
{%- elif transfer == "linear" %}
    return light;
{%- elif transfer == "log" %}
    float decades = transferCode() == 9u ? 2.0 : 2.5;

    return mix(1.0 + log(max(light, vec3(1e-30))) / log(10.0) / decades, vec3(0.0), lessThan(light, vec3(pow(10.0, -decades))));
{%- elif transfer == "pq" %}
    return pqSignal(light * 10000.0);
{%- elif transfer == "hlg" %}
    return hlgSignal(light);
{%- endif %}
}

vec3 inverseOetf(vec3 signal) {
{%- if transfer == "power" %}
    switch (transferCode()) {
        case 4u:
            return pow(max(signal, vec3(0.0)), vec3(2.2));
        case 5u:
            return pow(max(signal, vec3(0.0)), vec3(2.8));
        case 7u:
            return bt709Decode(signal, 1.1115, 0.0228, 4.0);
        case 17u:
            return pow(max(signal, vec3(0.0)), vec3(2.6)) * 52.37 / 48.0;
        default:
            return bt709Decode(signal, 1.099296826809442, 0.018053968510807, 4.5);
    }
{%- elif transfer == "srgb" %}
    vec3 v = max(signal, vec3(0.0));

    return mix(pow((v + 0.055) / 1.055, vec3(2.4)), v / 12.92, lessThanEqual(v, vec3(0.04045)));
{%- elif transfer == "linear" %}
    return signal;
{%- elif transfer == "log" %}
    float decades = transferCode() == 9u ? 2.0 : 2.5;

    return mix(pow(vec3(10.0), (signal - 1.0) * decades), vec3(0.0), lessThanEqual(signal, vec3(0.0)));
{%- elif transfer == "pq" %}
    return pqNits(signal) / 10000.0;
{%- elif transfer == "hlg" %}
    return hlgScene(signal);
{%- endif %}
}
{%- endif %}
{%- if source.model == "yuv" %}

vec2 lumaWeights() {
    switch (matrixCode()) {
        case 1u:
            return vec2(0.2126, 0.0722);
        case 4u:
            return vec2(0.30, 0.11);
        case 5u:
        case 6u:
            return vec2(0.299, 0.114);
        case 7u:
            return vec2(0.212, 0.087);
        case 9u:
        case 10u:
            return vec2(0.2627, 0.0593);
        case 12u:
        case 13u:
            return luminanceWeights().xz;
        default:
            return lumaSize().y > 576u ? vec2(0.2126, 0.0722) : vec2(0.299, 0.114);
    }
}

vec2 chromaAt(vec2 at) {
    uint location = frame.sites.x;
    vec2 within = location == 2u ? vec2(0.5, 0.5)
                : location == 3u ? vec2(0.0, 0.0)
                : location == 4u ? vec2(0.5, 0.0)
                : location == 5u ? vec2(0.0, 1.0)
                : location == 6u ? vec2(0.5, 1.0)
                : vec2(0.0, 0.5);
    vec2 scale = vec2(uvec2(1u) << chromaShift());

    return (at - within * (scale - 1.0)) / scale;
}
{%- if color == "linear" %}

mat3 yccToRgb() {
    switch (matrixCode()) {
        case 0u:
            return mat3(0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0);
        case 8u:
            return mat3(1.0, 1.0, 1.0, -1.0, 1.0, -1.0, 1.0, 0.0, -1.0);
        case 11u:
            return mat3(0.991902, 1.0, 1.0 / 0.986566, 0.0, 0.0, 2.0 / 0.986566, 2.0, 0.0, 0.0);
        case 16u:
        case 17u:
            return mat3(1.0, 1.0, 1.0, -0.5, 0.5, -0.5, 0.5, 0.0, -0.5);
        default: {
            vec2 k = lumaWeights();
            float kg = 1.0 - k.x - k.y;

            return mat3(1.0, 1.0, 1.0, 0.0, -2.0 * (1.0 - k.y) * k.y / kg, 2.0 * (1.0 - k.y), 2.0 * (1.0 - k.x), -2.0 * (1.0 - k.x) * k.x / kg, 0.0);
        }
    }
}

vec3 signalOf(vec3 ycc) {
    return yccToRgb() * ycc;
}
{%- elif color == "cl" %}

vec3 signalOf(vec3 ycc) {
    vec2 k = lumaWeights();
    vec3 negative = oetf(vec3(1.0 - k.y, 1.0 - k.x, 0.0));
    vec3 positive = 1.0 - oetf(vec3(k.y, k.x, 0.0));
    float b = ycc.x + 2.0 * ycc.y * (ycc.y <= 0.0 ? negative.x : positive.x);
    float r = ycc.x + 2.0 * ycc.z * (ycc.z <= 0.0 ? negative.y : positive.y);
    vec3 yrb = inverseOetf(vec3(ycc.x, r, b));
    float g = (yrb.x - k.x * yrb.y - k.y * yrb.z) / (1.0 - k.x - k.y);

    return vec3(r, oetf(vec3(g)).x, b);
}
{%- elif color == "ictcp" %}

vec3 signalOf(vec3 ictcp) {
    return ictcp;
}
{%- endif %}

vec4 signalAt(vec2 at) {
    vec2 chroma = chromaAt(at);
    float y = decode(sample0(at, lumaSize()), lumaLevels({{ components[0][4] }}u));
    float cb = decode(sample1(chroma, chromaSize()), chromaLevels({{ components[1][4] }}u));
    float cr = decode(sample2(chroma, chromaSize()), chromaLevels({{ components[2][4] }}u));
{%- if alpha %}
    float a = decode(sample3(at, lumaSize()), alphaLevels({{ components[3][4] }}u));
{%- else %}
    float a = 1.0;
{%- endif %}

    return vec4(signalOf(vec3(y, cb, cr)), a);
}
{%- elif source.model in ["rgb", "xyz"] %}

vec4 signalAt(vec2 at) {
    float r = decode(sample0(at, lumaSize()), lumaLevels({{ components[0][4] }}u));
    float g = decode(sample1(at, lumaSize()), lumaLevels({{ components[1][4] }}u));
    float b = decode(sample2(at, lumaSize()), lumaLevels({{ components[2][4] }}u));
{%- if alpha %}
    float a = decode(sample3(at, lumaSize()), alphaLevels({{ components[3][4] }}u));
{%- else %}
    float a = 1.0;
{%- endif %}

    return vec4(r, g, b, a);
}
{%- elif source.model == "gray" %}

vec4 signalAt(vec2 at) {
    float y = decode(sample0(at, lumaSize()), lumaLevels({{ components[0][4] }}u));
{%- if alpha %}
    float a = decode(sample1(at, lumaSize()), alphaLevels({{ components[1][4] }}u));
{%- else %}
    float a = 1.0;
{%- endif %}

    return vec4(y, y, y, a);
}
{%- elif source.model == "palette" %}

vec4 paletteColor(uvec2 at) {
    uint entry = words[frame.planeWord[1] + uint(code(at))];

    return vec4((entry >> 16) & 255u, (entry >> 8) & 255u, entry & 255u, entry >> 24) / 255.0;
}

vec4 signalAt(vec2 at) {
    uvec2 a;
    uvec2 b;
    vec2 f = taps(at, lumaSize(), a, b);
    vec4 top = mix(paletteColor(a), paletteColor(uvec2(b.x, a.y)), f.x);
    vec4 bottom = mix(paletteColor(uvec2(a.x, b.y)), paletteColor(b), f.x);

    return mix(top, bottom, f.y);
}
{%- elif source.model == "bayer" %}

uvec2 mirrored(ivec2 at) {
    ivec2 last = ivec2(lumaSize()) - 1;
    ivec2 reflected = abs(at);

    return uvec2(last - abs(last - reflected));
}

float mosaic(ivec2 at) {
    return code(mirrored(at));
}

vec3 demosaic(ivec2 at) {
    ivec2 red = ivec2(frame.sites.yz);
    ivec2 phase = at & 1;
    float here = mosaic(at);
    float horizontal = (mosaic(at - ivec2(1, 0)) + mosaic(at + ivec2(1, 0))) / 2.0;
    float vertical = (mosaic(at - ivec2(0, 1)) + mosaic(at + ivec2(0, 1))) / 2.0;
    float cross = (horizontal + vertical) / 2.0;
    float diagonal = (mosaic(at + ivec2(-1, -1)) + mosaic(at + ivec2(1, -1)) + mosaic(at + ivec2(-1, 1)) + mosaic(at + ivec2(1, 1))) / 4.0;

    if (phase == red) {
        return vec3(here, cross, diagonal);
    }

    if (phase == 1 - red) {
        return vec3(diagonal, cross, here);
    }

    return phase.y == red.y ? vec3(horizontal, here, vertical) : vec3(vertical, here, horizontal);
}

vec4 signalAt(vec2 at) {
    uvec2 a;
    uvec2 b;
    vec2 f = taps(at, lumaSize(), a, b);
    vec3 top = mix(demosaic(ivec2(a)), demosaic(ivec2(b.x, a.y)), f.x);
    vec3 bottom = mix(demosaic(ivec2(a.x, b.y)), demosaic(ivec2(b)), f.x);
    vec2 levels = lumaLevels({{ 8 * components[0][1] }}u);

    return vec4((mix(top, bottom, f.y) - levels.x) / levels.y, 1.0);
}
{%- endif %}

vec3 light(vec3 signal) {
{%- if color == "ictcp" and transfer == "pq" %}
    vec3 lms = pqNits({{ inverse_matrix([[2048, 2048, 0], [6610, -13613, 7003], [17933, -17390, -543]], 4096) }} * signal);

    return {{ inverse_matrix([[1688, 2146, 262], [683, 2951, 462], [99, 309, 3688]], 4096) }} * lms / frame.white.x;
{%- elif color == "ictcp" and transfer == "hlg" %}
    vec3 lms = hlgScene({{ inverse_matrix([[2048, 2048, 0], [3625, -7465, 3840], [9500, -9212, -288]], 4096) }} * signal);

    return hlgNits({{ inverse_matrix([[1688, 2146, 262], [683, 2951, 462], [99, 309, 3688]], 4096) }} * lms) / frame.white.x;
{%- else %}
    return eotf(signal);
{%- endif %}
}

vec4 encodeOutput(vec3 linear, float alpha) {
    vec3 xyz = primariesToXyz() * linear;
{%- if output == "hdr" %}
    vec3 rgb = {{ xyz_to_rgb(primaries[9]) }} * xyz;

    return vec4(rgb, alpha);
{%- elif output == "sdr" %}
    vec3 rgb = clamp({{ xyz_to_rgb(primaries[1]) }} * xyz, 0.0, 1.0);

    return vec4(mix(1.055 * pow(rgb, vec3(1.0 / 2.4)) - 0.055, rgb * 12.92, lessThanEqual(rgb, vec3(0.0031308))), alpha);
{%- endif %}
}

void main() {
    vec4 color = signalAt(vUv * vec2(lumaSize()) - 0.5);

    fColor = encodeOutput(light(color.rgb), color.a);
}
