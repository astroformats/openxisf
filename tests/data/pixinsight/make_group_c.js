// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz
//
// Writes the samples of group C with PixInsight: the 37 x 23 pattern image of sample A2 (Gray, UInt16), saved with
// block checksums, and twice with Zstandard compression and byte shuffling as well. The XISF module of PixInsight 1.9.4
// writes SHA-1, SHA-256 and SHA-512 checksums only, so C1 to C3 and C6 came from 1.9.4; the SHA-3 samples C4, C5 and C7
// came from PixInsight 1.9.6, whose module writes SHA3-256 and SHA3-512 checksums too.
// tests/samples/sample_catalog.cpp describes the samples. Run it in automation mode, for example on Windows:
//
//   PixInsight.exe -n --automation-mode -r="make_group_c.js,<output directory>" --force-exit
//
// It writes a log, group_c.log, next to the samples.

var outputDirectory = (typeof jsArguments !== "undefined" && jsArguments.length > 0) ?
    jsArguments[0] : File.extractDirectory(#__FILE__);

var log = "";

function note(text)
{
    log += text + "\n";
}

var commonHints = "block-alignment 4096 properties fits-keywords";

var samples = [
    { file: "c1-gray-u16-sha1.xisf", hints: "no-compression checksums sha1" },
    { file: "c2-gray-u16-sha256.xisf", hints: "no-compression checksums sha256" },
    { file: "c3-gray-u16-sha512.xisf", hints: "no-compression checksums sha512" },
    { file: "c4-gray-u16-sha3-256.xisf", hints: "no-compression checksums sha3-256" },
    { file: "c5-gray-u16-sha3-512.xisf", hints: "no-compression checksums sha3-512" },
    { file: "c6-gray-u16-zstd-sh-sha256.xisf", hints: "compression-codec zstd+sh checksums sha256" },
    { file: "c7-gray-u16-zstd-sh-sha3-256.xisf", hints: "compression-codec zstd+sh checksums sha3-256" }
];

// The image of sample A2: channel 0 of the pattern, which increases in storage order.
function createImage(id)
{
    var P = new PixelMath;
    P.expression = "(x() + 37*y())/850";
    P.useSingleExpression = true;
    P.rescale = false;
    P.truncate = true;
    P.createNewImage = true;
    P.showNewImage = true;
    P.newImageId = id;
    P.newImageWidth = 37;
    P.newImageHeight = 23;
    P.newImageAlpha = false;
    P.newImageColorSpace = PixelMath.prototype.Gray;
    P.newImageSampleFormat = PixelMath.prototype.i16;
    if (!P.executeGlobal())
        throw new Error("PixelMath failed");
    var window = ImageWindow.windowById(id);
    if (window.isNull)
        throw new Error("PixelMath created no image");
    return window;
}

try
{
    if (typeof coreVersionMajor !== "undefined")
        note("PixInsight " + coreVersionMajor + "." + coreVersionMinor + "." + coreVersionRelease + "-" +
             coreVersionRevision + " build " + coreVersionBuild);
    // One image for every sample, so that they all hold the same pixels.
    var window = createImage("openxisf_sample_c");
    for (var i = 0; i < samples.length; ++i)
    {
        var path = outputDirectory + "/" + samples[i].file;
        var hints = samples[i].hints + " " + commonHints;
        if (!window.saveAs(path, false, false, false, false, hints))
            throw new Error("cannot save " + path);
        note("wrote " + path + " with the format hints '" + hints + "'");
    }
    window.forceClose();
    note("done");
}
catch (failure)
{
    note("error: " + failure);
}

File.writeTextFile(outputDirectory + "/group_c.log", log);
