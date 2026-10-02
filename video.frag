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
{%- set systems = {
    "yuv": ["linear", "cl", "ictcp"],
    "rgb": ["rgb"],
    "gray": ["gray"],
    "xyz": ["xyz"],
    "palette": ["palette"],
    "bayer": ["bayer"],
} %}
{%- set shapes = {
    "linear": ["curve", "log", "pq", "hlg"],
    "cl": ["curve", "log", "pq", "hlg"],
    "ictcp": ["pq", "hlg"],
    "rgb": ["curve", "log", "pq", "hlg"],
    "gray": ["curve", "log", "pq", "hlg"],
    "xyz": ["curve", "log", "pq", "hlg"],
    "palette": ["curve", "log", "pq", "hlg"],
    "bayer": ["curve", "log", "pq", "hlg"],
} %}
{%- set outputs = ["hdr", "sdr"] %}
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
{%- set bt709 = [1 / 0.45, 0.099296826809442, 4.5 * 0.018053968510807, 4.5, 1] %}
{%- set transfers = {
    1: {"shape": "curve", "eotf": [2.4, 0, -1, 1, 1], "oetf": bt709},
    2: {"shape": "curve", "eotf": [2.4, 0, -1, 1, 1], "oetf": bt709},
    4: {"shape": "curve", "eotf": [2.2, 0, -1, 1, 1], "oetf": [2.2, 0, -1, 1, 1]},
    5: {"shape": "curve", "eotf": [2.8, 0, -1, 1, 1], "oetf": [2.8, 0, -1, 1, 1]},
    6: {"shape": "curve", "eotf": [2.4, 0, -1, 1, 1], "oetf": bt709},
    7: {"shape": "curve", "eotf": [2.4, 0, -1, 1, 1], "oetf": [1 / 0.45, 0.1115, 4 * 0.0228, 4, 1]},
    8: {"shape": "curve", "eotf": [1, 0, -1, 1, 1], "oetf": [1, 0, -1, 1, 1]},
    9: {"shape": "log", "eotf": [2, 0, 0, 0, 0], "oetf": [2, 0, 0, 0, 0]},
    10: {"shape": "log", "eotf": [2.5, 0, 0, 0, 0], "oetf": [2.5, 0, 0, 0, 0]},
    11: {"shape": "curve", "eotf": [2.4, 0, -1, 1, 1], "oetf": bt709},
    12: {"shape": "curve", "eotf": [2.4, 0, -1, 1, 1], "oetf": bt709},
    13: {"shape": "curve", "eotf": [2.4, 0.055, 0.04045, 12.92, 1], "oetf": [2.4, 0.055, 0.04045, 12.92, 1]},
    14: {"shape": "curve", "eotf": [2.4, 0, -1, 1, 1], "oetf": bt709},
    15: {"shape": "curve", "eotf": [2.4, 0, -1, 1, 1], "oetf": bt709},
    16: {"shape": "pq", "eotf": [0, 0, 0, 0, 0], "oetf": [0, 0, 0, 0, 0]},
    17: {"shape": "curve", "eotf": [2.6, 0, -1, 1, 52.37 / 48], "oetf": [2.6, 0, -1, 1, 52.37 / 48]},
    18: {"shape": "hlg", "eotf": [0, 0, 0, 0, 0], "oetf": [0, 0, 0, 0, 0]},
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
{%- set source = layouts[layout] %}
{%- set components = source.components %}
{%- set bigEndian = "be" in source.flags %}

{%- macro hex(value) -%}
0x{{ "%x" | format(value) }}u
{%- endmacro %}

{%- macro row(plane, at) -%}
frame.planeWord[{{ plane }}] + {{ at }}.y * frame.lineWords[{{ plane }}]
{%- endmacro %}

{%- macro plus(value) -%}
{%- if value %} + {{ value }}u{% endif -%}
{%- endmacro %}

{%- macro swap(value, bytes) -%}
{%- if bytes == 4 -%}
(({{ value }} & 0xffu) << 24 | ({{ value }} & 0xff00u) << 8 | {{ value }} >> 8 & 0xff00u | {{ value }} >> 24)
{%- else -%}
(({{ value }} & 0xffu) << 8 | {{ value }} >> 8 & 0xffu)
{%- endif -%}
{%- endmacro %}

{%- macro read(c, plane, step, offset, shift, depth, at) %}
{%- set bits = shift + depth %}
{%- set bytes = 1 if bits <= 8 else 2 if bits <= 16 else 4 %}
{%- set start = offset + 1 if bigEndian and bytes == 1 else offset %}
{%- set mask = hex(2 ** depth - 1) %}
{%- if "bits" in source.flags and depth == 10 %}
    uint word = words[{{ row(plane, at) }} + {{ at }}.x];
    uint value = {{ swap("word", 4) }};

    return float((value >> {{ offset }}u) & {{ mask }});
{%- elif "bits" in source.flags %}
    uint bit = {{ at }}.x * {{ step }}u{{ plus(offset) }};
    uint byte = bit >> 3;
    uint word = words[{{ row(plane, at) }} + (byte >> 2)];
    uint value = (word >> ((byte & 3u) * 8u + {{ 8 - depth }}u - (bit & 7u))) & {{ mask }};
{%- if "inverted" in source %}

    return float({{ mask }} - value);
{%- else %}

    return float(value);
{%- endif %}
{%- else %}
{%- if c == 0 and "luma" in source %}
    const uint group[4] = uint[]({{ source.luma[1] | join("u, ") }}u);
    uint byte = ({{ at }}.x >> 2) * {{ source.luma[0] }}u + group[{{ at }}.x & 3u];
    uint word = words[{{ row(plane, at) }} + (byte >> 2)];
    uint window = word >> ((byte & 3u) * 8u);
{%- elif step == 4 %}
    uint word = words[{{ row(plane, at) }} + {{ at }}.x];
    uint window = word{% if start %} >> {{ 8 * start }}u{% endif %};
{%- elif step % 4 == 0 %}
    uint word = words[{{ row(plane, at) }} + {{ at }}.x * {{ step // 4 }}u{{ plus(start // 4) }}];
    uint window = word{% if start % 4 %} >> {{ 8 * (start % 4) }}u{% endif %};
{%- elif step == 2 %}
    uint word = words[{{ row(plane, at) }} + ({{ at }}.x >> 1)];
    uint window = word >> (({{ at }}.x & 1u) * 16u{{ plus(8 * start) }});
{%- elif step == 1 %}
    uint word = words[{{ row(plane, at) }} + ({{ at }}.x >> 2)];
    uint window = word >> (({{ at }}.x & 3u) * 8u);
{%- else %}
    uint byte = {{ at }}.x * {{ step }}u{{ plus(start) }};
    uint word = words[{{ row(plane, at) }} + (byte >> 2)];
    uint window = word >> ((byte & 3u) * 8u);
{%- endif %}
{%- if "float" in source.flags and depth == 16 %}

    return unpackHalf2x16({{ swap("window", 2) if bigEndian else "window" }}).x;
{%- elif "float" in source.flags %}

    return uintBitsToFloat({{ swap("window", 4) if bigEndian else "window" }});
{%- elif bigEndian and bytes > 1 %}
    uint value = {{ swap("window", bytes) }};

    return float({% if shift %}(value >> {{ shift }}u) & {% else %}value & {% endif %}{{ mask }});
{%- else %}

    return float({% if shift %}(window >> {{ shift }}u) & {% else %}window & {% endif %}{{ mask }});
{%- endif %}
{%- endif %}
{%- endmacro %}

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
    vec4 sites;
    vec4 levelOffset;
    vec4 levelScale;
    mat3 toSignal;
    vec4 lumaWeights;
    vec4 eotf;
    vec4 oetf;
    vec4 scales;
    mat3 toOutput;
    vec4 luminance;
} frame;

vec2 taps(vec2 at, uvec2 extent, out uvec2 a, out uvec2 b) {
    vec2 base = floor(at);
    ivec2 last = ivec2(extent) - 1;

    a = uvec2(clamp(ivec2(base), ivec2(0), last));
    b = uvec2(clamp(ivec2(base) + 1, ivec2(0), last));

    return at - base;
}
{%- if source.model == "bayer" %}

float mosaic(ivec2 at) {
    ivec2 last = ivec2(frame.size.xy) - 1;
    uvec2 inside = uvec2(last - abs(last - abs(at)));
{{- read(0, 0, components[0][1], 0, 0, 8 * components[0][1], "inside") }}
}
{%- elif source.model == "palette" %}

vec4 paletteColor(uvec2 at) {
    uint word = words[{{ row(0, "at") }} + (at.x >> 2)];
    uint entry = words[frame.planeWord[1] + ((word >> ((at.x & 3u) * 8u)) & 0xffu)];

    return vec4(entry >> 16 & 0xffu, entry >> 8 & 0xffu, entry & 0xffu, entry >> 24) / 255.0;
}
{%- else %}
{%- for plane, step, offset, shift, depth in components %}

float code{{ loop.index0 }}(uvec2 at) {
{{- read(loop.index0, plane, step, offset, shift, depth, "at") }}
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

vec3 pqNits(vec3 signal) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    vec3 p = pow(clamp(signal, 0.0, 1.0), vec3(1.0 / m2));

    return pow(max(p - c1, 0.0) / (c2 - c3 * p), vec3(1.0 / m1)) * 10000.0;
}

vec3 hlgScene(vec3 signal) {
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    vec3 v = clamp(signal, 0.0, 1.0);

    return mix((exp((v - c) / a) + b) / 12.0, v * v / 3.0, lessThanEqual(v, vec3(0.5)));
}

vec3 hlgNits(vec3 scene) {
    const float peak = 1000.0;
    const float systemGamma = 1.2;
    float luminance = max(dot(scene, frame.luminance.xyz), 0.0);

    return peak * pow(luminance, systemGamma - 1.0) * scene;
}
{%- if transfer == "curve" %}

vec3 decodeCurve(vec3 signal, vec4 curve, float scale) {
    vec3 v = max(signal, 0.0);

    return scale * mix(pow((v + curve.y) / (1.0 + curve.y), vec3(curve.x)), v / curve.w, lessThanEqual(v, vec3(curve.z)));
}
{%- endif %}
{%- if system == "cl" %}

vec3 encodeLight(vec3 light) {
{%- if transfer == "curve" %}
    vec3 v = max(light / frame.scales.y, 0.0);

    return mix((1.0 + frame.oetf.y) * pow(v, vec3(1.0 / frame.oetf.x)) - frame.oetf.y, v * frame.oetf.w, lessThanEqual(v, vec3(frame.oetf.z / frame.oetf.w)));
{%- elif transfer == "log" %}
    vec3 v = max(light, vec3(1e-30));

    return mix(1.0 + log(v) / log(10.0) / frame.oetf.x, vec3(0.0), lessThan(v, vec3(pow(10.0, -frame.oetf.x))));
{%- elif transfer == "pq" %}
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    vec3 y = pow(clamp(light, 0.0, 1.0), vec3(m1));

    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
{%- elif transfer == "hlg" %}
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    vec3 v = max(light, 0.0);

    return mix(a * log(max(12.0 * v - b, 1e-6)) + c, sqrt(3.0 * v), lessThanEqual(v, vec3(1.0 / 12.0)));
{%- endif %}
}

vec3 decodeLight(vec3 signal) {
{%- if transfer == "curve" %}
    return decodeCurve(signal, frame.oetf, frame.scales.y);
{%- elif transfer == "log" %}
    return mix(pow(vec3(10.0), (signal - 1.0) * frame.oetf.x), vec3(0.0), lessThanEqual(signal, vec3(0.0)));
{%- elif transfer == "pq" %}
    return pqNits(signal) / 10000.0;
{%- elif transfer == "hlg" %}
    return hlgScene(signal);
{%- endif %}
}
{%- endif %}
{%- if source.model == "yuv" %}

vec3 systemSignal(vec3 ycc) {
{%- if system == "linear" %}
    return frame.toSignal * ycc;
{%- elif system == "cl" %}
    vec2 k = frame.lumaWeights.xy;
    vec2 negative = encodeLight(vec3(1.0 - k.y, 1.0 - k.x, 0.0)).xy;
    vec2 positive = 1.0 - encodeLight(vec3(k.y, k.x, 0.0)).xy;
    vec2 chroma = 2.0 * ycc.yz * mix(positive, negative, lessThanEqual(ycc.yz, vec2(0.0)));
    vec3 yrb = vec3(ycc.x, ycc.x + chroma.y, ycc.x + chroma.x);
    vec3 light = decodeLight(yrb);
    float g = (light.x - k.x * light.y - k.y * light.z) / (1.0 - k.x - k.y);

    return vec3(yrb.y, encodeLight(vec3(g)).x, yrb.z);
{%- elif system == "ictcp" %}
    return ycc;
{%- endif %}
}

vec4 signalAt(vec2 at) {
    uvec2 shift = frame.size.zw;
    vec2 chroma = (at - frame.sites.xy) / vec2(uvec2(1u) << shift);
    uvec2 chromaSize = (frame.size.xy + (uvec2(1u) << shift) - 1u) >> shift;
{%- if "alpha" in source.flags %}
    vec4 codes = vec4(sample0(at, frame.size.xy), sample1(chroma, chromaSize), sample2(chroma, chromaSize), sample3(at, frame.size.xy));
{%- else %}
    vec4 codes = vec4(sample0(at, frame.size.xy), sample1(chroma, chromaSize), sample2(chroma, chromaSize), 1.0);
{%- endif %}
    vec4 values = (codes - frame.levelOffset) / frame.levelScale;

    return vec4(systemSignal(values.xyz), values.w);
}
{%- elif source.model in ["rgb", "xyz"] %}

vec4 signalAt(vec2 at) {
{%- if "alpha" in source.flags %}
    vec4 codes = vec4(sample0(at, frame.size.xy), sample1(at, frame.size.xy), sample2(at, frame.size.xy), sample3(at, frame.size.xy));
{%- else %}
    vec4 codes = vec4(sample0(at, frame.size.xy), sample1(at, frame.size.xy), sample2(at, frame.size.xy), 1.0);
{%- endif %}

    return (codes - frame.levelOffset) / frame.levelScale;
}
{%- elif source.model == "gray" %}

vec4 signalAt(vec2 at) {
{%- if "alpha" in source.flags %}
    vec2 codes = vec2(sample0(at, frame.size.xy), sample1(at, frame.size.xy));
{%- else %}
    vec2 codes = vec2(sample0(at, frame.size.xy), 1.0);
{%- endif %}
    vec2 values = (codes - frame.levelOffset.xw) / frame.levelScale.xw;

    return values.xxxy;
}
{%- elif source.model == "palette" %}

vec4 signalAt(vec2 at) {
    uvec2 a;
    uvec2 b;
    vec2 f = taps(at, frame.size.xy, a, b);
    vec4 top = mix(paletteColor(a), paletteColor(uvec2(b.x, a.y)), f.x);
    vec4 bottom = mix(paletteColor(uvec2(a.x, b.y)), paletteColor(b), f.x);

    return mix(top, bottom, f.y);
}
{%- elif source.model == "bayer" %}

vec3 demosaic(ivec2 at) {
    ivec2 red = ivec2(frame.sites.zw);
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

vec4 signalAt(vec2 at) {
    uvec2 a;
    uvec2 b;
    vec2 f = taps(at, frame.size.xy, a, b);
    vec3 top = mix(demosaic(ivec2(a)), demosaic(ivec2(b.x, a.y)), f.x);
    vec3 bottom = mix(demosaic(ivec2(a.x, b.y)), demosaic(ivec2(b)), f.x);

    return vec4((mix(top, bottom, f.y) - frame.levelOffset.xyz) / frame.levelScale.xyz, 1.0);
}
{%- endif %}

vec3 light(vec3 signal) {
{%- if system == "ictcp" and transfer == "pq" %}
    vec3 lms = pqNits({{ inverse_matrix([[2048, 2048, 0], [6610, -13613, 7003], [17933, -17390, -543]], 4096) }} * signal);

    return {{ inverse_matrix([[1688, 2146, 262], [683, 2951, 462], [99, 309, 3688]], 4096) }} * lms / frame.scales.z;
{%- elif system == "ictcp" and transfer == "hlg" %}
    vec3 lms = hlgScene({{ inverse_matrix([[2048, 2048, 0], [3625, -7465, 3840], [9500, -9212, -288]], 4096) }} * signal);

    return hlgNits({{ inverse_matrix([[1688, 2146, 262], [683, 2951, 462], [99, 309, 3688]], 4096) }} * lms) / frame.scales.z;
{%- elif transfer == "curve" %}
    return decodeCurve(signal, frame.eotf, frame.scales.x);
{%- elif transfer == "log" %}
    return mix(pow(vec3(10.0), (signal - 1.0) * frame.eotf.x), vec3(0.0), lessThanEqual(signal, vec3(0.0)));
{%- elif transfer == "pq" %}
    return pqNits(signal) / frame.scales.z;
{%- elif transfer == "hlg" %}
    return hlgNits(hlgScene(signal)) / frame.scales.z;
{%- endif %}
}

void main() {
    vec4 color = signalAt(vUv * vec2(frame.size.xy) - 0.5);
    vec3 rgb = frame.toOutput * light(color.rgb);
{%- if output == "hdr" %}

    fColor = vec4(rgb, color.a);
{%- elif output == "sdr" %}
    vec3 v = clamp(rgb, 0.0, 1.0);

    fColor = vec4(mix(1.055 * pow(v, vec3(1.0 / 2.4)) - 0.055, v * 12.92, lessThanEqual(v, vec3(0.0031308))), color.a);
{%- endif %}
}
