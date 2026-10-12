// SDK patch 0008 (patches/rexglue) adds an application-supplied guest output
// to rex::ui::Presenter. These wrappers compile against an SDK without the
// patch too (built with -SkipSdk before the patch existed); the native
// renderer preview then reports that the SDK needs rebuilding.

#pragma once

#include <cstdint>
#include <functional>
#include <utility>

#include <rex/ui/presenter.h>

namespace amped3::live {

template <typename P = rex::ui::Presenter>
constexpr bool PresenterHasExternalOutput() {
  return requires(P* p) {
    p->SetGuestOutputExternal(true);
    p->IsGuestOutputExternal();
  };
}

template <typename P>
bool IsGuestOutputExternal(P* p) {
  if constexpr (requires { p->IsGuestOutputExternal(); }) {
    return p->IsGuestOutputExternal();
  } else {
    return false;
  }
}

template <typename P>
void SetGuestOutputExternal(P* p, bool external) {
  if constexpr (requires { p->SetGuestOutputExternal(external); }) {
    p->SetGuestOutputExternal(external);
  }
}

template <typename P, typename F>
bool RefreshGuestOutputExternal(P* p, uint32_t width, uint32_t height, F&& refresher) {
  if constexpr (requires { p->RefreshGuestOutputExternal(width, height, width, height, refresher); }) {
    return p->RefreshGuestOutputExternal(width, height, width, height,
                                         std::forward<F>(refresher));
  } else {
    return false;
  }
}

}  // namespace amped3::live
