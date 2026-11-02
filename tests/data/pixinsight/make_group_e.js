// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz
//
// Writes the samples of group E with PixInsight 1.9.6 or later, whose XISF module stores a compression subblock that
// its codec does not make shorter as it is, with equal compressed and uncompressed lengths (spec §10.6.1, Revision 2),
// divides a block into subblocks of 4 MiB, and writes a block none of whose subblocks gets shorter without compression:
//
// - E1 to E4: a 2048 x 1025 Gray UInt16 image (4,198,400 bytes), a ramp along x whose last two rows (4096 samples) are
//   noise, saved with zstd+sh, lz4+sh, zstd, and zstd+sh with SHA-256 checksums. Byte shuffling puts the low bytes of
//   every sample first, so the last 4096 bytes of the shuffled block, beyond the first 4 MiB, are the high bytes of
//   the noise: the second subblock, which no codec makes shorter, is stored as it is.
// - E5: a 64 x 64 Gray UInt16 image of noise saved with zstd+sh, which PixInsight writes uncompressed.
//
// tests/samples/sample_catalog.cpp describes the samples. Run it in automation mode, for example on Windows:
//
//   PixInsight.exe -n --automation-mode -r="make_group_e.js,<output directory>" --force-exit
//
// It writes a log, group_e.log, next to the samples.

var outputDirectory = (typeof jsArguments !== "undefined" && jsArguments.length > 0) ?
    jsArguments[0] : File.extractDirectory(#__FILE__);

var log = "";

function note(text)
{
    log += text + "\n";
}

var commonHints = "block-alignment 4096 properties fits-keywords";

var samples = [
    { file: "e1-gray-u16-zstd-sh-stored.xisf", image: "ramp", hints: "compression-codec zstd+sh no-checksums" },
    { file: "e2-gray-u16-lz4-sh-stored.xisf", image: "ramp", hints: "compression-codec lz4+sh no-checksums" },
    { file: "e3-gray-u16-zstd-stored.xisf", image: "ramp", hints: "compression-codec zstd no-checksums" },
    { file: "e4-gray-u16-zstd-sh-stored-sha256.xisf", image: "ramp",
      hints: "compression-codec zstd+sh checksums sha256" },
    { file: "e5-gray-u16-zstd-sh-noise.xisf", image: "noise", hints: "compression-codec zstd+sh no-checksums" }
];

// A Gray UInt16 image of the given expression, without rescaling and with truncation, as the samples of group A.
function createImage(id, width, height, expression)
{
    var P = new PixelMath;
    P.expression = expression;
    P.useSingleExpression = true;
    P.rescale = false;
    P.truncate = true;
    P.createNewImage = true;
    P.showNewImage = true;
    P.newImageId = id;
    P.newImageWidth = width;
    P.newImageHeight = height;
    P.newImageAlpha = false;
    P.newImageColorSpace = PixelMath.prototype.Gray;
    P.newImageSampleFormat = PixelMath.prototype.i16;
    if (!P.executeGlobal())
        throw new Error("PixelMath failed for " + id);
    var window = ImageWindow.windowById(id);
    if (window.isNull)
        throw new Error("PixelMath created no image " + id);
    return window;
}

try
{
    if (typeof coreVersionMajor !== "undefined")
        note("PixInsight " + coreVersionMajor + "." + coreVersionMinor + "." + coreVersionRelease + "-" +
             coreVersionRevision + " build " + coreVersionBuild);
    // One image of each kind, so that E1 to E4 hold the same pixels, noise included.
    var images = {
        ramp: createImage("openxisf_sample_e_ramp", 2048, 1025, "iif(y() >= 1023, random(), x()/2047)"),
        noise: createImage("openxisf_sample_e_noise", 64, 64, "random()")
    };
    for (var i = 0; i < samples.length; ++i)
    {
        var path = outputDirectory + "/" + samples[i].file;
        var hints = samples[i].hints + " " + commonHints;
        if (!images[samples[i].image].saveAs(path, false, false, false, false, hints))
            throw new Error("cannot save " + path);
        note("wrote " + path + " with the format hints '" + hints + "'");
    }
    images.ramp.forceClose();
    images.noise.forceClose();
    note("done");
}
catch (failure)
{
    note("error: " + failure);
}

File.writeTextFile(outputDirectory + "/group_e.log", log);
