// amped3 - ReXGlue Recompiled Project

#include "generated/default/amped3_init.h"

#include "amped3_app.h"

#include <cstdlib>

std::unique_ptr<rex::ui::WindowedApp> Amped3App::Create(rex::ui::WindowedAppContext& ctx) {
  // Already running (a second launch): leave the first one alone.
  if (!amped3::AcquireSingleInstance()) std::exit(0);
  return std::unique_ptr<Amped3App>(new Amped3App(ctx, "amped3", PPCImageConfig));
}

REX_DEFINE_APP(amped3, Amped3App::Create)
