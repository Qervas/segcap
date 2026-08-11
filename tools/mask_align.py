"""Align an object-id mask to the colour frame it belongs to.

The mask and the frame do not necessarily have the same size, and which one is
bigger is a property of the game, not of our capture:

  Stray            scene renders at the backbuffer resolution.  mask == frame.
  inZOI            r.ScreenPercentage=50 with DLSS, so the 3D scene -- and
                   therefore CustomDepth, and therefore the mask -- renders at
                   1280x800 while the swapchain presents 2560x1600.

segcap copies the mask out of the render target it was written to, so the mask
carries the SCENE resolution (readback.cpp: width_ = desc.Width). The colour
frame comes from the swapchain backbuffer. On a 50%-screen-percentage title
those differ by exactly 2x.

Both make_demo.py and overlay.py used to reconcile this by shrinking the FRAME
down to the mask, silently, through PIL's default resample. That is wrong twice
over: it throws away three quarters of the video the demo is supposed to show,
and it makes a real misalignment invisible by construction, because any two
sizes can be made to agree by resizing one of them.

Upscale the MASK instead, and only by an exact integer factor.

WHY NEAREST-NEIGHBOUR IS NOT A PREFERENCE HERE. Mask values are object ids, not
intensities. Averaging id 60 and id 158 gives 109, which is either a different
object or no object at all -- the interpolated pixels would be confident,
plausible, and false, which is the failure mode this project has already been
burned by. np.repeat is exact: it replicates each id into a kxk block and can
be inverted by slicing. No resample argument, no PIL, nothing to get wrong.
"""

import numpy as np


class AlignmentError(Exception):
    """The mask cannot be aligned to the frame without inventing pixels."""


def align_mask_to_frame(mask, frame_size):
    """Return `mask` upscaled to `frame_size` == (width, height).

    Raises AlignmentError when the sizes are not related by a single integer
    factor in both axes -- refusing is correct there, because a non-integer
    ratio means these are not the same frame, or the capture geometry changed
    mid-run, and no amount of resampling makes the result trustworthy.
    """
    fw, fh = frame_size
    mh, mw = mask.shape[:2]

    if (mw, mh) == (fw, fh):
        return mask

    if fw < mw or fh < mh:
        raise AlignmentError(
            "frame is %dx%d but mask is %dx%d -- the frame is SMALLER. Downscaling a "
            "mask would merge neighbouring object ids into whichever one happened to "
            "be sampled; refusing." % (fw, fh, mw, mh))

    if fw % mw or fh % mh:
        raise AlignmentError(
            "frame is %dx%d but mask is %dx%d -- ratio %.4fx%.4f is not an integer. "
            "These are probably not the same frame; refusing to resample object ids."
            % (fw, fh, mw, mh, fw / mw, fh / mh))

    kx, ky = fw // mw, fh // mh
    if kx != ky:
        raise AlignmentError(
            "frame is %dx%d but mask is %dx%d -- anisotropic ratio %dx%d. A non-square "
            "pixel scaling means the aspect changed between the scene target and the "
            "backbuffer; refusing." % (fw, fh, mw, mh, kx, ky))

    return np.repeat(np.repeat(mask, ky, axis=0), kx, axis=1)
