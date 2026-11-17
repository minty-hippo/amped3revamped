// amped3-subtitles: FFmpeg's Bink video decoder (bink.c, with binkdsp.c,
// hpeldsp.c and blockdsp.c) built into the subtitle maker for the editor's
// movie preview. The SDK's FFmpeg build has none of these; their per-CPU
// speed-ups aren't built either, so the plain C versions are used: these
// stand in for the CPU-specific setup FFmpeg would call.

#include "config.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/blockdsp.h"
#include "libavcodec/hpeldsp.h"

#if ARCH_X86
void ff_hpeldsp_init_x86(HpelDSPContext* c, int flags) {}
void ff_blockdsp_init_x86(BlockDSPContext* c, AVCodecContext* avctx) {}
#endif
#if ARCH_AARCH64
void ff_hpeldsp_init_aarch64(HpelDSPContext* c, int flags) {}
#endif
#if ARCH_ARM
void ff_hpeldsp_init_arm(HpelDSPContext* c, int flags) {}
void ff_blockdsp_init_arm(BlockDSPContext* c) {}
#endif
#if ARCH_PPC
void ff_hpeldsp_init_ppc(HpelDSPContext* c, int flags) {}
void ff_blockdsp_init_ppc(BlockDSPContext* c) {}
#endif
#if ARCH_MIPS
void ff_hpeldsp_init_mips(HpelDSPContext* c, int flags) {}
void ff_blockdsp_init_mips(BlockDSPContext* c) {}
#endif
#if ARCH_ALPHA
void ff_hpeldsp_init_alpha(HpelDSPContext* c, int flags) {}
void ff_blockdsp_init_alpha(BlockDSPContext* c) {}
#endif

// (bink.c's decoder, for C++.)
extern AVCodec ff_bink_decoder;
const AVCodec* amped3_bink_video_decoder(void) { return &ff_bink_decoder; }
