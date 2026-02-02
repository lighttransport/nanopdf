// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// CFF (Compact Font Format) Parser implementation
// Reference: Adobe Technical Note #5176 "The Compact Font Format Specification"

#include "cff-parser.hh"

#include <algorithm>
#include <cstring>

namespace nanopdf {
namespace cff {

namespace {

// CFF Standard Strings (SID 0-390)
// These are predefined strings that don't need to be stored in the String INDEX
const char* kStandardStrings[] = {
    ".notdef",          // 0
    "space",            // 1
    "exclam",           // 2
    "quotedbl",         // 3
    "numbersign",       // 4
    "dollar",           // 5
    "percent",          // 6
    "ampersand",        // 7
    "quoteright",       // 8
    "parenleft",        // 9
    "parenright",       // 10
    "asterisk",         // 11
    "plus",             // 12
    "comma",            // 13
    "hyphen",           // 14
    "period",           // 15
    "slash",            // 16
    "zero",             // 17
    "one",              // 18
    "two",              // 19
    "three",            // 20
    "four",             // 21
    "five",             // 22
    "six",              // 23
    "seven",            // 24
    "eight",            // 25
    "nine",             // 26
    "colon",            // 27
    "semicolon",        // 28
    "less",             // 29
    "equal",            // 30
    "greater",          // 31
    "question",         // 32
    "at",               // 33
    "A",                // 34
    "B",                // 35
    "C",                // 36
    "D",                // 37
    "E",                // 38
    "F",                // 39
    "G",                // 40
    "H",                // 41
    "I",                // 42
    "J",                // 43
    "K",                // 44
    "L",                // 45
    "M",                // 46
    "N",                // 47
    "O",                // 48
    "P",                // 49
    "Q",                // 50
    "R",                // 51
    "S",                // 52
    "T",                // 53
    "U",                // 54
    "V",                // 55
    "W",                // 56
    "X",                // 57
    "Y",                // 58
    "Z",                // 59
    "bracketleft",      // 60
    "backslash",        // 61
    "bracketright",     // 62
    "asciicircum",      // 63
    "underscore",       // 64
    "quoteleft",        // 65
    "a",                // 66
    "b",                // 67
    "c",                // 68
    "d",                // 69
    "e",                // 70
    "f",                // 71
    "g",                // 72
    "h",                // 73
    "i",                // 74
    "j",                // 75
    "k",                // 76
    "l",                // 77
    "m",                // 78
    "n",                // 79
    "o",                // 80
    "p",                // 81
    "q",                // 82
    "r",                // 83
    "s",                // 84
    "t",                // 85
    "u",                // 86
    "v",                // 87
    "w",                // 88
    "x",                // 89
    "y",                // 90
    "z",                // 91
    "braceleft",        // 92
    "bar",              // 93
    "braceright",       // 94
    "asciitilde",       // 95
    "exclamdown",       // 96
    "cent",             // 97
    "sterling",         // 98
    "fraction",         // 99
    "yen",              // 100
    "florin",           // 101
    "section",          // 102
    "currency",         // 103
    "quotesingle",      // 104
    "quotedblleft",     // 105
    "guillemotleft",    // 106
    "guilsinglleft",    // 107
    "guilsinglright",   // 108
    "fi",               // 109
    "fl",               // 110
    "endash",           // 111
    "dagger",           // 112
    "daggerdbl",        // 113
    "periodcentered",   // 114
    "paragraph",        // 115
    "bullet",           // 116
    "quotesinglbase",   // 117
    "quotedblbase",     // 118
    "quotedblright",    // 119
    "guillemotright",   // 120
    "ellipsis",         // 121
    "perthousand",      // 122
    "questiondown",     // 123
    "grave",            // 124
    "acute",            // 125
    "circumflex",       // 126
    "tilde",            // 127
    "macron",           // 128
    "breve",            // 129
    "dotaccent",        // 130
    "dieresis",         // 131
    "ring",             // 132
    "cedilla",          // 133
    "hungarumlaut",     // 134
    "ogonek",           // 135
    "caron",            // 136
    "emdash",           // 137
    "AE",               // 138
    "ordfeminine",      // 139
    "Lslash",           // 140
    "Oslash",           // 141
    "OE",               // 142
    "ordmasculine",     // 143
    "ae",               // 144
    "dotlessi",         // 145
    "lslash",           // 146
    "oslash",           // 147
    "oe",               // 148
    "germandbls",       // 149
    "onesuperior",      // 150
    "logicalnot",       // 151
    "mu",               // 152
    "trademark",        // 153
    "Eth",              // 154
    "onehalf",          // 155
    "plusminus",        // 156
    "Thorn",            // 157
    "onequarter",       // 158
    "divide",           // 159
    "brokenbar",        // 160
    "degree",           // 161
    "thorn",            // 162
    "threequarters",    // 163
    "twosuperior",      // 164
    "registered",       // 165
    "minus",            // 166
    "eth",              // 167
    "multiply",         // 168
    "threesuperior",    // 169
    "copyright",        // 170
    "Aacute",           // 171
    "Acircumflex",      // 172
    "Adieresis",        // 173
    "Agrave",           // 174
    "Aring",            // 175
    "Atilde",           // 176
    "Ccedilla",         // 177
    "Eacute",           // 178
    "Ecircumflex",      // 179
    "Edieresis",        // 180
    "Egrave",           // 181
    "Iacute",           // 182
    "Icircumflex",      // 183
    "Idieresis",        // 184
    "Igrave",           // 185
    "Ntilde",           // 186
    "Oacute",           // 187
    "Ocircumflex",      // 188
    "Odieresis",        // 189
    "Ograve",           // 190
    "Otilde",           // 191
    "Scaron",           // 192
    "Uacute",           // 193
    "Ucircumflex",      // 194
    "Udieresis",        // 195
    "Ugrave",           // 196
    "Yacute",           // 197
    "Ydieresis",        // 198
    "Zcaron",           // 199
    "aacute",           // 200
    "acircumflex",      // 201
    "adieresis",        // 202
    "agrave",           // 203
    "aring",            // 204
    "atilde",           // 205
    "ccedilla",         // 206
    "eacute",           // 207
    "ecircumflex",      // 208
    "edieresis",        // 209
    "egrave",           // 210
    "iacute",           // 211
    "icircumflex",      // 212
    "idieresis",        // 213
    "igrave",           // 214
    "ntilde",           // 215
    "oacute",           // 216
    "ocircumflex",      // 217
    "odieresis",        // 218
    "ograve",           // 219
    "otilde",           // 220
    "scaron",           // 221
    "uacute",           // 222
    "ucircumflex",      // 223
    "udieresis",        // 224
    "ugrave",           // 225
    "yacute",           // 226
    "ydieresis",        // 227
    "zcaron",           // 228
    "exclamsmall",      // 229
    "Hungarumlautsmall",// 230
    "dollaroldstyle",   // 231
    "dollarsuperior",   // 232
    "ampersandsmall",   // 233
    "Acutesmall",       // 234
    "parenleftsuperior",// 235
    "parenrightsuperior",// 236
    "twodotenleader",   // 237
    "onedotenleader",   // 238
    "zerooldstyle",     // 239
    "oneoldstyle",      // 240
    "twooldstyle",      // 241
    "threeoldstyle",    // 242
    "fouroldstyle",     // 243
    "fiveoldstyle",     // 244
    "sixoldstyle",      // 245
    "sevenoldstyle",    // 246
    "eightoldstyle",    // 247
    "nineoldstyle",     // 248
    "commasuperior",    // 249
    "threequartersemdash",// 250
    "periodsuperior",   // 251
    "questionsmall",    // 252
    "asuperior",        // 253
    "bsuperior",        // 254
    "centsuperior",     // 255
    "dsuperior",        // 256
    "esuperior",        // 257
    "isuperior",        // 258
    "lsuperior",        // 259
    "msuperior",        // 260
    "nsuperior",        // 261
    "osuperior",        // 262
    "rsuperior",        // 263
    "ssuperior",        // 264
    "tsuperior",        // 265
    "ff",               // 266
    "ffi",              // 267
    "ffl",              // 268
    "parenleftinferior",// 269
    "parenrightinferior",// 270
    "Circumflexsmall",  // 271
    "hyphensuperior",   // 272
    "Gravesmall",       // 273
    "Asmall",           // 274
    "Bsmall",           // 275
    "Csmall",           // 276
    "Dsmall",           // 277
    "Esmall",           // 278
    "Fsmall",           // 279
    "Gsmall",           // 280
    "Hsmall",           // 281
    "Ismall",           // 282
    "Jsmall",           // 283
    "Ksmall",           // 284
    "Lsmall",           // 285
    "Msmall",           // 286
    "Nsmall",           // 287
    "Osmall",           // 288
    "Psmall",           // 289
    "Qsmall",           // 290
    "Rsmall",           // 291
    "Ssmall",           // 292
    "Tsmall",           // 293
    "Usmall",           // 294
    "Vsmall",           // 295
    "Wsmall",           // 296
    "Xsmall",           // 297
    "Ysmall",           // 298
    "Zsmall",           // 299
    "colonmonetary",    // 300
    "onefitted",        // 301
    "rupiah",           // 302
    "Tildesmall",       // 303
    "exclamdownsmall",  // 304
    "centoldstyle",     // 305
    "Lslashsmall",      // 306
    "Scaronsmall",      // 307
    "Zcaronsmall",      // 308
    "Dieresissmall",    // 309
    "Brevesmall",       // 310
    "Caronsmall",       // 311
    "Dotaccentsmall",   // 312
    "Macronsmall",      // 313
    "figuredash",       // 314
    "hypheninferior",   // 315
    "Ogoneksmall",      // 316
    "Ringsmall",        // 317
    "Cedillasmall",     // 318
    "questiondownsmall",// 319
    "oneeighth",        // 320
    "threeeighths",     // 321
    "fiveeighths",      // 322
    "seveneighths",     // 323
    "onethird",         // 324
    "twothirds",        // 325
    "zerosuperior",     // 326
    "foursuperior",     // 327
    "fivesuperior",     // 328
    "sixsuperior",      // 329
    "sevensuperior",    // 330
    "eightsuperior",    // 331
    "ninesuperior",     // 332
    "zeroinferior",     // 333
    "oneinferior",      // 334
    "twoinferior",      // 335
    "threeinferior",    // 336
    "fourinferior",     // 337
    "fiveinferior",     // 338
    "sixinferior",      // 339
    "seveninferior",    // 340
    "eightinferior",    // 341
    "nineinferior",     // 342
    "centinferior",     // 343
    "dollarinferior",   // 344
    "periodinferior",   // 345
    "commainferior",    // 346
    "Agravesmall",      // 347
    "Aacutesmall",      // 348
    "Acircumflexsmall", // 349
    "Atildesmall",      // 350
    "Adieresissmall",   // 351
    "Aringsmall",       // 352
    "AEsmall",          // 353
    "Ccedillasmall",    // 354
    "Egravesmall",      // 355
    "Eacutesmall",      // 356
    "Ecircumflexsmall", // 357
    "Edieresissmall",   // 358
    "Igravesmall",      // 359
    "Iacutesmall",      // 360
    "Icircumflexsmall", // 361
    "Idieresissmall",   // 362
    "Ethsmall",         // 363
    "Ntildesmall",      // 364
    "Ogravesmall",      // 365
    "Oacutesmall",      // 366
    "Ocircumflexsmall", // 367
    "Otildesmall",      // 368
    "Odieresissmall",   // 369
    "OEsmall",          // 370
    "Oslashsmall",      // 371
    "Ugravesmall",      // 372
    "Uacutesmall",      // 373
    "Ucircumflexsmall", // 374
    "Udieresissmall",   // 375
    "Yacutesmall",      // 376
    "Thornsmall",       // 377
    "Ydieresissmall",   // 378
    "001.000",          // 379
    "001.001",          // 380
    "001.002",          // 381
    "001.003",          // 382
    "Black",            // 383
    "Bold",             // 384
    "Book",             // 385
    "Light",            // 386
    "Medium",           // 387
    "Regular",          // 388
    "Roman",            // 389
    "Semibold",         // 390
};

const int kNumStandardStrings = 391;

// Standard encoding: code -> SID
// Only non-zero entries are listed
const int kStandardEncoding[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0-15
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 16-31
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,  // 32-47 (space-slash)
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,  // 48-63 (0-?)
    33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,  // 64-79 (@-O)
    49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64,  // 80-95 (P-_)
    65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80,  // 96-111 (`-o)
    81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 0,   // 112-127 (p-DEL)
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 128-143
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 144-159
    0, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110,  // 160-175
    0, 111, 112, 113, 114, 0, 115, 116, 117, 118, 119, 120, 121, 122, 0, 123,  // 176-191
    0, 124, 125, 126, 127, 128, 129, 130, 131, 0, 132, 133, 0, 134, 135, 136,  // 192-207
    137, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 208-223
    0, 138, 0, 139, 0, 0, 0, 0, 140, 141, 142, 143, 0, 0, 0, 0,  // 224-239
    0, 144, 0, 0, 0, 145, 0, 0, 146, 147, 148, 149, 0, 0, 0, 0,  // 240-255
};

// Expert encoding: code -> SID
const int kExpertEncoding[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0-15
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 16-31
    1, 229, 230, 0, 231, 232, 233, 234, 235, 236, 237, 238, 13, 14, 15, 99,  // 32-47
    239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 27, 28, 249, 250, 251, 252,  // 48-63
    0, 253, 254, 255, 256, 257, 0, 0, 0, 258, 0, 0, 259, 260, 261, 262,  // 64-79
    0, 0, 263, 264, 265, 0, 266, 109, 110, 267, 268, 269, 0, 270, 271, 272,  // 80-95
    273, 274, 275, 276, 277, 278, 279, 280, 281, 282, 283, 284, 285, 286, 287, 288,  // 96-111
    289, 290, 291, 292, 293, 294, 295, 296, 297, 298, 299, 300, 301, 302, 303, 0,  // 112-127
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 128-143
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 144-159
    0, 304, 305, 306, 0, 0, 307, 308, 309, 310, 311, 0, 312, 0, 0, 313,  // 160-175
    0, 0, 314, 315, 0, 0, 316, 317, 318, 0, 0, 0, 158, 155, 163, 319,  // 176-191
    320, 321, 322, 323, 324, 325, 0, 0, 326, 150, 164, 169, 327, 328, 329, 330,  // 192-207
    331, 332, 333, 334, 335, 336, 337, 338, 339, 340, 341, 342, 343, 344, 345, 346,  // 208-223
    347, 348, 349, 350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 360, 361, 362,  // 224-239
    363, 364, 365, 366, 367, 368, 369, 370, 371, 372, 373, 374, 375, 376, 377, 378,  // 240-255
};

// ISO Adobe charset: glyph index -> SID (228 glyphs)
const int kISOAdobeCharset[] = {
    0,    // 0: .notdef
    1,    // 1: space
    2,    // 2: exclam
    3,    // 3: quotedbl
    4,    // 4: numbersign
    5,    // 5: dollar
    6,    // 6: percent
    7,    // 7: ampersand
    8,    // 8: quoteright
    9,    // 9: parenleft
    10,   // 10: parenright
    11,   // 11: asterisk
    12,   // 12: plus
    13,   // 13: comma
    14,   // 14: hyphen
    15,   // 15: period
    16,   // 16: slash
    17,   // 17: zero
    18,   // 18: one
    19,   // 19: two
    20,   // 20: three
    21,   // 21: four
    22,   // 22: five
    23,   // 23: six
    24,   // 24: seven
    25,   // 25: eight
    26,   // 26: nine
    27,   // 27: colon
    28,   // 28: semicolon
    29,   // 29: less
    30,   // 30: equal
    31,   // 31: greater
    32,   // 32: question
    33,   // 33: at
    34,   // 34: A
    35,   // 35: B
    36,   // 36: C
    37,   // 37: D
    38,   // 38: E
    39,   // 39: F
    40,   // 40: G
    41,   // 41: H
    42,   // 42: I
    43,   // 43: J
    44,   // 44: K
    45,   // 45: L
    46,   // 46: M
    47,   // 47: N
    48,   // 48: O
    49,   // 49: P
    50,   // 50: Q
    51,   // 51: R
    52,   // 52: S
    53,   // 53: T
    54,   // 54: U
    55,   // 55: V
    56,   // 56: W
    57,   // 57: X
    58,   // 58: Y
    59,   // 59: Z
    60,   // 60: bracketleft
    61,   // 61: backslash
    62,   // 62: bracketright
    63,   // 63: asciicircum
    64,   // 64: underscore
    65,   // 65: quoteleft
    66,   // 66: a
    67,   // 67: b
    68,   // 68: c
    69,   // 69: d
    70,   // 70: e
    71,   // 71: f
    72,   // 72: g
    73,   // 73: h
    74,   // 74: i
    75,   // 75: j
    76,   // 76: k
    77,   // 77: l
    78,   // 78: m
    79,   // 79: n
    80,   // 80: o
    81,   // 81: p
    82,   // 82: q
    83,   // 83: r
    84,   // 84: s
    85,   // 85: t
    86,   // 86: u
    87,   // 87: v
    88,   // 88: w
    89,   // 89: x
    90,   // 90: y
    91,   // 91: z
    92,   // 92: braceleft
    93,   // 93: bar
    94,   // 94: braceright
    95,   // 95: asciitilde
    96,   // 96: exclamdown
    97,   // 97: cent
    98,   // 98: sterling
    99,   // 99: fraction
    100,  // 100: yen
    101,  // 101: florin
    102,  // 102: section
    103,  // 103: currency
    104,  // 104: quotesingle
    105,  // 105: quotedblleft
    106,  // 106: guillemotleft
    107,  // 107: guilsinglleft
    108,  // 108: guilsinglright
    109,  // 109: fi
    110,  // 110: fl
    111,  // 111: endash
    112,  // 112: dagger
    113,  // 113: daggerdbl
    114,  // 114: periodcentered
    115,  // 115: paragraph
    116,  // 116: bullet
    117,  // 117: quotesinglbase
    118,  // 118: quotedblbase
    119,  // 119: quotedblright
    120,  // 120: guillemotright
    121,  // 121: ellipsis
    122,  // 122: perthousand
    123,  // 123: questiondown
    124,  // 124: grave
    125,  // 125: acute
    126,  // 126: circumflex
    127,  // 127: tilde
    128,  // 128: macron
    129,  // 129: breve
    130,  // 130: dotaccent
    131,  // 131: dieresis
    132,  // 132: ring
    133,  // 133: cedilla
    134,  // 134: hungarumlaut
    135,  // 135: ogonek
    136,  // 136: caron
    137,  // 137: emdash
    138,  // 138: AE
    139,  // 139: ordfeminine
    140,  // 140: Lslash
    141,  // 141: Oslash
    142,  // 142: OE
    143,  // 143: ordmasculine
    144,  // 144: ae
    145,  // 145: dotlessi
    146,  // 146: lslash
    147,  // 147: oslash
    148,  // 148: oe
    149,  // 149: germandbls
    150,  // 150: onesuperior
    151,  // 151: logicalnot
    152,  // 152: mu
    153,  // 153: trademark
    154,  // 154: Eth
    155,  // 155: onehalf
    156,  // 156: plusminus
    157,  // 157: Thorn
    158,  // 158: onequarter
    159,  // 159: divide
    160,  // 160: brokenbar
    161,  // 161: degree
    162,  // 162: thorn
    163,  // 163: threequarters
    164,  // 164: twosuperior
    165,  // 165: registered
    166,  // 166: minus
    167,  // 167: eth
    168,  // 168: multiply
    169,  // 169: threesuperior
    170,  // 170: copyright
    171,  // 171: Aacute
    172,  // 172: Acircumflex
    173,  // 173: Adieresis
    174,  // 174: Agrave
    175,  // 175: Aring
    176,  // 176: Atilde
    177,  // 177: Ccedilla
    178,  // 178: Eacute
    179,  // 179: Ecircumflex
    180,  // 180: Edieresis
    181,  // 181: Egrave
    182,  // 182: Iacute
    183,  // 183: Icircumflex
    184,  // 184: Idieresis
    185,  // 185: Igrave
    186,  // 186: Ntilde
    187,  // 187: Oacute
    188,  // 188: Ocircumflex
    189,  // 189: Odieresis
    190,  // 190: Ograve
    191,  // 191: Otilde
    192,  // 192: Scaron
    193,  // 193: Uacute
    194,  // 194: Ucircumflex
    195,  // 195: Udieresis
    196,  // 196: Ugrave
    197,  // 197: Yacute
    198,  // 198: Ydieresis
    199,  // 199: Zcaron
    200,  // 200: aacute
    201,  // 201: acircumflex
    202,  // 202: adieresis
    203,  // 203: agrave
    204,  // 204: aring
    205,  // 205: atilde
    206,  // 206: ccedilla
    207,  // 207: eacute
    208,  // 208: ecircumflex
    209,  // 209: edieresis
    210,  // 210: egrave
    211,  // 211: iacute
    212,  // 212: icircumflex
    213,  // 213: idieresis
    214,  // 214: igrave
    215,  // 215: ntilde
    216,  // 216: oacute
    217,  // 217: ocircumflex
    218,  // 218: odieresis
    219,  // 219: ograve
    220,  // 220: otilde
    221,  // 221: scaron
    222,  // 222: uacute
    223,  // 223: ucircumflex
    224,  // 224: udieresis
    225,  // 225: ugrave
    226,  // 226: yacute
    227,  // 227: ydieresis
    228,  // 228: zcaron
};

const int kISOAdobeCharsetSize = 229;

// Expert charset: glyph index -> SID (166 glyphs)
const int kExpertCharset[] = {
    0,    // 0: .notdef
    1,    // 1: space
    229,  // 2: exclamsmall
    230,  // 3: Hungarumlautsmall
    231,  // 4: dollaroldstyle
    232,  // 5: dollarsuperior
    233,  // 6: ampersandsmall
    234,  // 7: Acutesmall
    235,  // 8: parenleftsuperior
    236,  // 9: parenrightsuperior
    237,  // 10: twodotenleader
    238,  // 11: onedotenleader
    13,   // 12: comma
    14,   // 13: hyphen
    15,   // 14: period
    99,   // 15: fraction
    239,  // 16: zerooldstyle
    240,  // 17: oneoldstyle
    241,  // 18: twooldstyle
    242,  // 19: threeoldstyle
    243,  // 20: fouroldstyle
    244,  // 21: fiveoldstyle
    245,  // 22: sixoldstyle
    246,  // 23: sevenoldstyle
    247,  // 24: eightoldstyle
    248,  // 25: nineoldstyle
    27,   // 26: colon
    28,   // 27: semicolon
    249,  // 28: commasuperior
    250,  // 29: threequartersemdash
    251,  // 30: periodsuperior
    252,  // 31: questionsmall
    253,  // 32: asuperior
    254,  // 33: bsuperior
    255,  // 34: centsuperior
    256,  // 35: dsuperior
    257,  // 36: esuperior
    258,  // 37: isuperior
    259,  // 38: lsuperior
    260,  // 39: msuperior
    261,  // 40: nsuperior
    262,  // 41: osuperior
    263,  // 42: rsuperior
    264,  // 43: ssuperior
    265,  // 44: tsuperior
    266,  // 45: ff
    267,  // 46: ffi
    268,  // 47: ffl
    269,  // 48: parenleftinferior
    270,  // 49: parenrightinferior
    271,  // 50: Circumflexsmall
    272,  // 51: hyphensuperior
    273,  // 52: Gravesmall
    274,  // 53: Asmall
    275,  // 54: Bsmall
    276,  // 55: Csmall
    277,  // 56: Dsmall
    278,  // 57: Esmall
    279,  // 58: Fsmall
    280,  // 59: Gsmall
    281,  // 60: Hsmall
    282,  // 61: Ismall
    283,  // 62: Jsmall
    284,  // 63: Ksmall
    285,  // 64: Lsmall
    286,  // 65: Msmall
    287,  // 66: Nsmall
    288,  // 67: Osmall
    289,  // 68: Psmall
    290,  // 69: Qsmall
    291,  // 70: Rsmall
    292,  // 71: Ssmall
    293,  // 72: Tsmall
    294,  // 73: Usmall
    295,  // 74: Vsmall
    296,  // 75: Wsmall
    297,  // 76: Xsmall
    298,  // 77: Ysmall
    299,  // 78: Zsmall
    300,  // 79: colonmonetary
    301,  // 80: onefitted
    302,  // 81: rupiah
    303,  // 82: Tildesmall
    304,  // 83: exclamdownsmall
    305,  // 84: centoldstyle
    306,  // 85: Lslashsmall
    307,  // 86: Scaronsmall
    308,  // 87: Zcaronsmall
    309,  // 88: Dieresissmall
    310,  // 89: Brevesmall
    311,  // 90: Caronsmall
    312,  // 91: Dotaccentsmall
    313,  // 92: Macronsmall
    314,  // 93: figuredash
    315,  // 94: hypheninferior
    316,  // 95: Ogoneksmall
    317,  // 96: Ringsmall
    318,  // 97: Cedillasmall
    319,  // 98: questiondownsmall
    320,  // 99: oneeighth
    321,  // 100: threeeighths
    322,  // 101: fiveeighths
    323,  // 102: seveneighths
    324,  // 103: onethird
    325,  // 104: twothirds
    326,  // 105: zerosuperior
    327,  // 106: foursuperior
    328,  // 107: fivesuperior
    329,  // 108: sixsuperior
    330,  // 109: sevensuperior
    331,  // 110: eightsuperior
    332,  // 111: ninesuperior
    333,  // 112: zeroinferior
    334,  // 113: oneinferior
    335,  // 114: twoinferior
    336,  // 115: threeinferior
    337,  // 116: fourinferior
    338,  // 117: fiveinferior
    339,  // 118: sixinferior
    340,  // 119: seveninferior
    341,  // 120: eightinferior
    342,  // 121: nineinferior
    343,  // 122: centinferior
    344,  // 123: dollarinferior
    345,  // 124: periodinferior
    346,  // 125: commainferior
    347,  // 126: Agravesmall
    348,  // 127: Aacutesmall
    349,  // 128: Acircumflexsmall
    350,  // 129: Atildesmall
    351,  // 130: Adieresissmall
    352,  // 131: Aringsmall
    353,  // 132: AEsmall
    354,  // 133: Ccedillasmall
    355,  // 134: Egravesmall
    356,  // 135: Eacutesmall
    357,  // 136: Ecircumflexsmall
    358,  // 137: Edieresissmall
    359,  // 138: Igravesmall
    360,  // 139: Iacutesmall
    361,  // 140: Icircumflexsmall
    362,  // 141: Idieresissmall
    363,  // 142: Ethsmall
    364,  // 143: Ntildesmall
    365,  // 144: Ogravesmall
    366,  // 145: Oacutesmall
    367,  // 146: Ocircumflexsmall
    368,  // 147: Otildesmall
    369,  // 148: Odieresissmall
    370,  // 149: OEsmall
    371,  // 150: Oslashsmall
    372,  // 151: Ugravesmall
    373,  // 152: Uacutesmall
    374,  // 153: Ucircumflexsmall
    375,  // 154: Udieresissmall
    376,  // 155: Yacutesmall
    377,  // 156: Thornsmall
    378,  // 157: Ydieresissmall
};

const int kExpertCharsetSize = 166;

// Expert Subset charset: glyph index -> SID (87 glyphs)
const int kExpertSubsetCharset[] = {
    0,    // 0: .notdef
    1,    // 1: space
    231,  // 2: dollaroldstyle
    232,  // 3: dollarsuperior
    235,  // 4: parenleftsuperior
    236,  // 5: parenrightsuperior
    237,  // 6: twodotenleader
    238,  // 7: onedotenleader
    13,   // 8: comma
    14,   // 9: hyphen
    15,   // 10: period
    99,   // 11: fraction
    239,  // 12: zerooldstyle
    240,  // 13: oneoldstyle
    241,  // 14: twooldstyle
    242,  // 15: threeoldstyle
    243,  // 16: fouroldstyle
    244,  // 17: fiveoldstyle
    245,  // 18: sixoldstyle
    246,  // 19: sevenoldstyle
    247,  // 20: eightoldstyle
    248,  // 21: nineoldstyle
    27,   // 22: colon
    28,   // 23: semicolon
    249,  // 24: commasuperior
    250,  // 25: threequartersemdash
    251,  // 26: periodsuperior
    253,  // 27: asuperior
    254,  // 28: bsuperior
    255,  // 29: centsuperior
    256,  // 30: dsuperior
    257,  // 31: esuperior
    258,  // 32: isuperior
    259,  // 33: lsuperior
    260,  // 34: msuperior
    261,  // 35: nsuperior
    262,  // 36: osuperior
    263,  // 37: rsuperior
    264,  // 38: ssuperior
    265,  // 39: tsuperior
    266,  // 40: ff
    267,  // 41: ffi
    268,  // 42: ffl
    269,  // 43: parenleftinferior
    270,  // 44: parenrightinferior
    272,  // 45: hyphensuperior
    300,  // 46: colonmonetary
    301,  // 47: onefitted
    302,  // 48: rupiah
    305,  // 49: centoldstyle
    314,  // 50: figuredash
    315,  // 51: hypheninferior
    320,  // 52: oneeighth
    321,  // 53: threeeighths
    322,  // 54: fiveeighths
    323,  // 55: seveneighths
    324,  // 56: onethird
    325,  // 57: twothirds
    326,  // 58: zerosuperior
    327,  // 59: foursuperior
    328,  // 60: fivesuperior
    329,  // 61: sixsuperior
    330,  // 62: sevensuperior
    331,  // 63: eightsuperior
    332,  // 64: ninesuperior
    333,  // 65: zeroinferior
    334,  // 66: oneinferior
    335,  // 67: twoinferior
    336,  // 68: threeinferior
    337,  // 69: fourinferior
    338,  // 70: fiveinferior
    339,  // 71: sixinferior
    340,  // 72: seveninferior
    341,  // 73: eightinferior
    342,  // 74: nineinferior
    343,  // 75: centinferior
    344,  // 76: dollarinferior
    345,  // 77: periodinferior
    346,  // 78: commainferior
};

const int kExpertSubsetCharsetSize = 87;

// Helper to read 1-4 byte big-endian integers
inline uint8_t read_u8(const uint8_t* data, size_t& pos) {
  return data[pos++];
}

inline uint16_t read_u16(const uint8_t* data, size_t& pos) {
  uint16_t val = (static_cast<uint16_t>(data[pos]) << 8) |
                  static_cast<uint16_t>(data[pos + 1]);
  pos += 2;
  return val;
}

inline uint32_t read_u24(const uint8_t* data, size_t& pos) {
  uint32_t val = (static_cast<uint32_t>(data[pos]) << 16) |
                 (static_cast<uint32_t>(data[pos + 1]) << 8) |
                  static_cast<uint32_t>(data[pos + 2]);
  pos += 3;
  return val;
}

inline uint32_t read_u32(const uint8_t* data, size_t& pos) {
  uint32_t val = (static_cast<uint32_t>(data[pos]) << 24) |
                 (static_cast<uint32_t>(data[pos + 1]) << 16) |
                 (static_cast<uint32_t>(data[pos + 2]) << 8) |
                  static_cast<uint32_t>(data[pos + 3]);
  pos += 4;
  return val;
}

// Read offset based on offset size (1-4 bytes)
inline uint32_t read_offset(const uint8_t* data, size_t& pos, int offSize) {
  switch (offSize) {
    case 1: return read_u8(data, pos);
    case 2: return read_u16(data, pos);
    case 3: return read_u24(data, pos);
    case 4: return read_u32(data, pos);
    default: return 0;
  }
}

}  // namespace

// Public API functions
const char* get_standard_string(int sid) {
  if (sid >= 0 && sid < kNumStandardStrings) {
    return kStandardStrings[sid];
  }
  return nullptr;
}

const char* standard_encoding_name(int code) {
  if (code >= 0 && code < 256) {
    int sid = kStandardEncoding[code];
    if (sid > 0 && sid < kNumStandardStrings) {
      return kStandardStrings[sid];
    }
  }
  return nullptr;
}

const char* expert_encoding_name(int code) {
  if (code >= 0 && code < 256) {
    int sid = kExpertEncoding[code];
    if (sid > 0 && sid < kNumStandardStrings) {
      return kStandardStrings[sid];
    }
  }
  return nullptr;
}

const char* iso_adobe_charset_name(int glyph_index) {
  if (glyph_index >= 0 && glyph_index < kISOAdobeCharsetSize) {
    int sid = kISOAdobeCharset[glyph_index];
    if (sid >= 0 && sid < kNumStandardStrings) {
      return kStandardStrings[sid];
    }
  }
  return nullptr;
}

// CFFParser implementation
bool CFFParser::parse(const uint8_t* data, size_t size, CFFData& result) {
  if (!data || size < 4) {
    return false;
  }

  data_ = data;
  size_ = size;
  pos_ = 0;

  // Parse CFF header
  // Header: major(1) minor(1) hdrSize(1) offSize(1)
  uint8_t major = data_[pos_++];
  uint8_t minor = data_[pos_++];
  uint8_t hdrSize = data_[pos_++];
  uint8_t offSize = data_[pos_++];

  (void)major;
  (void)minor;
  (void)offSize;

  // Skip to end of header
  pos_ = hdrSize;

  // Parse Name INDEX
  size_t name_count;
  if (!read_index(name_count, name_index_offsets_)) {
    return false;
  }

  // Extract font name from Name INDEX
  if (name_count > 0 && name_index_offsets_.size() >= 2) {
    size_t start = name_index_offsets_[0];
    size_t end = name_index_offsets_[1];
    if (end > start && end <= size_) {
      result.font_name = std::string(reinterpret_cast<const char*>(data_ + start),
                                     end - start);
    }
  }

  // Parse Top DICT INDEX
  size_t top_dict_count;
  if (!read_index(top_dict_count, top_dict_index_offsets_)) {
    return false;
  }

  // Parse String INDEX
  if (!parse_string_index()) {
    return false;
  }

  // Skip Global Subr INDEX
  size_t gsubr_count;
  std::vector<size_t> gsubr_offsets;
  if (!read_index(gsubr_count, gsubr_offsets)) {
    return false;
  }

  // Parse the first Top DICT to get charset and encoding offsets
  if (top_dict_count > 0 && top_dict_index_offsets_.size() >= 2) {
    size_t start = top_dict_index_offsets_[0];
    size_t length = top_dict_index_offsets_[1] - start;
    if (!parse_top_dict(data_ + start, length, result)) {
      return false;
    }
  }

  // Parse charset
  if (charset_offset_ == 0) {
    // ISOAdobe charset (predefined)
    result.charset.resize(kISOAdobeCharsetSize);
    for (int i = 0; i < kISOAdobeCharsetSize; ++i) {
      result.charset[i] = get_sid_string(kISOAdobeCharset[i]);
    }
  } else if (charset_offset_ == 1) {
    // Expert charset (predefined)
    result.charset.resize(kExpertCharsetSize);
    for (int i = 0; i < kExpertCharsetSize; ++i) {
      result.charset[i] = get_sid_string(kExpertCharset[i]);
    }
  } else if (charset_offset_ == 2) {
    // Expert Subset charset (predefined)
    result.charset.resize(kExpertSubsetCharsetSize);
    for (int i = 0; i < kExpertSubsetCharsetSize; ++i) {
      result.charset[i] = get_sid_string(kExpertSubsetCharset[i]);
    }
  } else if (charset_offset_ > 0 && charset_offset_ < size_) {
    // Custom charset
    pos_ = charset_offset_;
    uint8_t format = read_u8(data_, pos_);
    if (!parse_charset(format, result.num_glyphs, result)) {
      // Fall back to ISOAdobe
      result.charset.resize(kISOAdobeCharsetSize);
      for (int i = 0; i < kISOAdobeCharsetSize; ++i) {
        result.charset[i] = get_sid_string(kISOAdobeCharset[i]);
      }
    }
  }

  // Parse encoding
  if (encoding_offset_ == 0) {
    // Standard encoding (predefined)
    result.encoding.resize(256, 0);
    for (int i = 0; i < 256; ++i) {
      result.encoding[i] = kStandardEncoding[i];
    }
  } else if (encoding_offset_ == 1) {
    // Expert encoding (predefined)
    result.encoding.resize(256, 0);
    for (int i = 0; i < 256; ++i) {
      result.encoding[i] = kExpertEncoding[i];
    }
  } else if (encoding_offset_ > 0 && encoding_offset_ < size_) {
    // Custom encoding
    pos_ = encoding_offset_;
    uint8_t format = read_u8(data_, pos_);
    if (!parse_encoding(format, result)) {
      // Fall back to standard encoding
      result.encoding.resize(256, 0);
      for (int i = 0; i < 256; ++i) {
        result.encoding[i] = kStandardEncoding[i];
      }
    }
  }

  return true;
}

bool CFFParser::read_dict_operand(const uint8_t* data, size_t size, size_t& pos,
                                  std::vector<double>& operands) {
  if (pos >= size) {
    return false;
  }

  uint8_t b0 = data[pos++];

  if (b0 >= 32 && b0 <= 246) {
    // Single byte integer: value = b0 - 139
    operands.push_back(static_cast<double>(static_cast<int>(b0) - 139));
    return true;
  } else if (b0 >= 247 && b0 <= 250) {
    // Two byte positive integer
    if (pos >= size) return false;
    uint8_t b1 = data[pos++];
    int value = (static_cast<int>(b0) - 247) * 256 + static_cast<int>(b1) + 108;
    operands.push_back(static_cast<double>(value));
    return true;
  } else if (b0 >= 251 && b0 <= 254) {
    // Two byte negative integer
    if (pos >= size) return false;
    uint8_t b1 = data[pos++];
    int value = -(static_cast<int>(b0) - 251) * 256 - static_cast<int>(b1) - 108;
    operands.push_back(static_cast<double>(value));
    return true;
  } else if (b0 == 28) {
    // 3-byte integer (signed 16-bit)
    if (pos + 1 >= size) return false;
    int16_t value = static_cast<int16_t>((data[pos] << 8) | data[pos + 1]);
    pos += 2;
    operands.push_back(static_cast<double>(value));
    return true;
  } else if (b0 == 29) {
    // 5-byte integer (signed 32-bit)
    if (pos + 3 >= size) return false;
    int32_t value = static_cast<int32_t>(
        (static_cast<uint32_t>(data[pos]) << 24) |
        (static_cast<uint32_t>(data[pos + 1]) << 16) |
        (static_cast<uint32_t>(data[pos + 2]) << 8) |
         static_cast<uint32_t>(data[pos + 3]));
    pos += 4;
    operands.push_back(static_cast<double>(value));
    return true;
  } else if (b0 == 30) {
    // Real number (BCD encoded)
    std::string realStr;
    bool done = false;
    while (!done && pos < size) {
      uint8_t byte = data[pos++];
      for (int i = 0; i < 2 && !done; ++i) {
        uint8_t nibble = (i == 0) ? (byte >> 4) : (byte & 0x0F);
        switch (nibble) {
          case 0x0: case 0x1: case 0x2: case 0x3: case 0x4:
          case 0x5: case 0x6: case 0x7: case 0x8: case 0x9:
            realStr += static_cast<char>('0' + nibble);
            break;
          case 0xA:  // Decimal point
            realStr += '.';
            break;
          case 0xB:  // Positive exponent
            realStr += 'E';
            break;
          case 0xC:  // Negative exponent
            realStr += "E-";
            break;
          case 0xD:  // Reserved
            break;
          case 0xE:  // Minus sign
            realStr += '-';
            break;
          case 0xF:  // End of number
            done = true;
            break;
        }
      }
    }
    if (!realStr.empty()) {
      operands.push_back(std::stod(realStr));
    }
    return true;
  }

  return false;  // Unknown operand type
}

bool CFFParser::read_index(size_t& count, std::vector<size_t>& offsets) {
  if (pos_ + 2 > size_) {
    return false;
  }

  // Read count (Card16)
  count = read_u16(data_, pos_);

  if (count == 0) {
    offsets.clear();
    return true;
  }

  if (pos_ >= size_) {
    return false;
  }

  // Read offSize (OffSize)
  uint8_t offSize = read_u8(data_, pos_);
  if (offSize < 1 || offSize > 4) {
    return false;
  }

  // Read offset array (count + 1 offsets)
  offsets.resize(count + 1);
  size_t dataStart = pos_ + (count + 1) * offSize;

  for (size_t i = 0; i <= count; ++i) {
    uint32_t offset = read_offset(data_, pos_, offSize);
    // Offsets are 1-based relative to data start
    offsets[i] = dataStart + offset - 1;
  }

  // Move position to end of INDEX data
  if (count > 0 && offsets.size() > count) {
    pos_ = offsets[count];
  }

  return true;
}

bool CFFParser::parse_top_dict(const uint8_t* data, size_t size, CFFData& result) {
  std::vector<double> operands;
  size_t pos = 0;

  while (pos < size) {
    uint8_t b0 = data[pos];

    // Check if this is an operator
    if (b0 <= 21) {
      pos++;  // Skip operator byte

      int op = b0;
      if (b0 == 12) {
        // Two-byte operator
        if (pos >= size) return false;
        op = 0x0C00 | data[pos++];
      }

      // Process operator with current operands
      switch (op) {
        case 0:  // version (SID)
          if (!operands.empty()) {
            result.version = get_sid_string(static_cast<int>(operands[0]));
          }
          break;
        case 1:  // Notice (SID)
          if (!operands.empty()) {
            result.notice = get_sid_string(static_cast<int>(operands[0]));
          }
          break;
        case 0x0C00:  // Copyright (SID) - operator 12 0
          if (!operands.empty()) {
            result.copyright = get_sid_string(static_cast<int>(operands[0]));
          }
          break;
        case 2:  // FullName (SID)
          if (!operands.empty()) {
            result.full_name = get_sid_string(static_cast<int>(operands[0]));
          }
          break;
        case 3:  // FamilyName (SID)
          if (!operands.empty()) {
            result.family_name = get_sid_string(static_cast<int>(operands[0]));
          }
          break;
        case 4:  // Weight (SID)
          if (!operands.empty()) {
            result.weight = get_sid_string(static_cast<int>(operands[0]));
          }
          break;
        case 5:  // FontBBox (array)
          result.font_bbox = operands;
          break;
        case 0x0C07:  // FontMatrix (array) - operator 12 7
          result.font_matrix = operands;
          break;
        case 15:  // charset (offset)
          if (!operands.empty()) {
            charset_offset_ = static_cast<size_t>(operands[0]);
          }
          break;
        case 16:  // Encoding (offset)
          if (!operands.empty()) {
            encoding_offset_ = static_cast<size_t>(operands[0]);
          }
          break;
        case 17:  // CharStrings (offset)
          if (!operands.empty()) {
            charstrings_offset_ = static_cast<size_t>(operands[0]);
          }
          break;
        case 0x0C1E:  // ROS (Registry-Ordering-Supplement) - CID font
          result.is_cid = true;
          break;
        default:
          break;
      }

      operands.clear();
    } else {
      // This is an operand
      if (!read_dict_operand(data, size, pos, operands)) {
        return false;
      }
    }
  }

  // Get number of glyphs from CharStrings INDEX
  if (charstrings_offset_ > 0 && charstrings_offset_ < size_) {
    size_t saved_pos = pos_;
    pos_ = charstrings_offset_;
    if (pos_ + 2 <= size_) {
      result.num_glyphs = read_u16(data_, pos_);
    }
    pos_ = saved_pos;
  }

  return true;
}

bool CFFParser::parse_string_index() {
  size_t count;
  std::vector<size_t> offsets;

  if (!read_index(count, offsets)) {
    return false;
  }

  string_index_offsets_ = offsets;
  strings_.resize(count);

  for (size_t i = 0; i < count; ++i) {
    size_t start = offsets[i];
    size_t end = offsets[i + 1];
    if (start < size_ && end <= size_ && end > start) {
      strings_[i] = std::string(reinterpret_cast<const char*>(data_ + start),
                                end - start);
    }
  }

  return true;
}

std::string CFFParser::get_sid_string(int sid) {
  if (sid < kNumStandardStrings) {
    const char* str = kStandardStrings[sid];
    return str ? str : "";
  }

  // Look up in String INDEX
  int index = sid - kNumStandardStrings;
  if (index >= 0 && index < static_cast<int>(strings_.size())) {
    return strings_[index];
  }

  return "";
}

bool CFFParser::parse_charset(int format, int num_glyphs, CFFData& result) {
  result.charset.clear();
  result.charset.push_back(".notdef");  // GID 0 is always .notdef

  if (num_glyphs <= 1) {
    return true;
  }

  int remaining = num_glyphs - 1;

  switch (format) {
    case 0: {
      // Format 0: Simple array of SIDs
      for (int i = 0; i < remaining && pos_ + 2 <= size_; ++i) {
        int sid = read_u16(data_, pos_);
        result.charset.push_back(get_sid_string(sid));
      }
      break;
    }
    case 1: {
      // Format 1: Ranges with 1-byte count
      while (remaining > 0 && pos_ + 3 <= size_) {
        int first = read_u16(data_, pos_);
        int nLeft = read_u8(data_, pos_);

        for (int i = 0; i <= nLeft && remaining > 0; ++i, --remaining) {
          result.charset.push_back(get_sid_string(first + i));
        }
      }
      break;
    }
    case 2: {
      // Format 2: Ranges with 2-byte count
      while (remaining > 0 && pos_ + 4 <= size_) {
        int first = read_u16(data_, pos_);
        int nLeft = read_u16(data_, pos_);

        for (int i = 0; i <= nLeft && remaining > 0; ++i, --remaining) {
          result.charset.push_back(get_sid_string(first + i));
        }
      }
      break;
    }
    default:
      return false;
  }

  return true;
}

bool CFFParser::parse_encoding(int format, CFFData& result) {
  result.encoding.resize(256, 0);

  int baseFormat = format & 0x7F;
  bool hasSupplement = (format & 0x80) != 0;

  switch (baseFormat) {
    case 0: {
      // Format 0: Simple array
      if (pos_ >= size_) return false;
      int nCodes = read_u8(data_, pos_);

      for (int i = 0; i < nCodes && pos_ < size_; ++i) {
        int code = read_u8(data_, pos_);
        if (code < 256) {
          result.encoding[code] = i + 1;  // GID 0 is .notdef
        }
      }
      break;
    }
    case 1: {
      // Format 1: Ranges
      if (pos_ >= size_) return false;
      int nRanges = read_u8(data_, pos_);

      int gid = 1;  // Start from GID 1 (GID 0 is .notdef)
      for (int i = 0; i < nRanges && pos_ + 2 <= size_; ++i) {
        int first = read_u8(data_, pos_);
        int nLeft = read_u8(data_, pos_);

        for (int j = 0; j <= nLeft && first + j < 256; ++j) {
          result.encoding[first + j] = gid++;
        }
      }
      break;
    }
    default:
      return false;
  }

  // Parse supplemental encoding if present
  if (hasSupplement && pos_ < size_) {
    int nSups = read_u8(data_, pos_);

    for (int i = 0; i < nSups && pos_ + 3 <= size_; ++i) {
      int code = read_u8(data_, pos_);
      int sid = read_u16(data_, pos_);
      (void)sid;  // SID supplements are stored but not used here
      // The supplement maps code to SID, but we're tracking code to GID
      // For now, we just skip the supplement data
    }
  }

  return true;
}

}  // namespace cff
}  // namespace nanopdf
