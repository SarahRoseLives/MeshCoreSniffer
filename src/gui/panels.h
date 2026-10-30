// panels.h - fixed dashboard panels.
#pragma once

#include <imgui.h>

#include "app.h"

namespace mcs::gui {

void drawMenuBar(AppState& s);
void drawHeader(AppState& s);
void drawToolbar(AppState& s);
void drawSidebar(AppState& s);
void drawMapPanel(AppState& s, const ImVec2& size);
void drawMainTabs(AppState& s);
void drawStatusBar(AppState& s);
void drawModals(AppState& s);

} // namespace mcs::gui
