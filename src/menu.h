// In-game settings panel, without an overlay library: the text is rendered by Windows into a small bitmap,
// uploaded when it changes. It is drawn on the finished frame at Present (present.cpp); if that is not
// possible, the colour stage draws it into the game's frame instead.
#pragma once
#include "colorstage.h"

namespace d5 {

struct Menu {
    bool visible = false;
    // Reads the keyboard (only while the game's window is in front) and applies changes to the settings.
    void input();
    // Returns the panel to draw this frame, or nullptr while it is closed. Records the upload on `cl`
    // when the content changed; touches no pipeline state.
    const Panel* prepare(ID3D12GraphicsCommandList* cl, UINT frameW, UINT frameH, const char* status);

    // For the panel drawn at Present: the panel's rectangle in a frame of that size, and its pixels when
    // they changed (or when `force`). False while the panel is closed.
    bool snapshot(UINT frameW, UINT frameH, bool force, std::vector<uint32_t>& pixels, UINT& x, UINT& y, UINT& w, UINT& h);
    void setStatus(const char* status);

private:
    int selected = 0;
    bool dirty = true;
    uint32_t seenGeneration = 0;
    char lastStatus[200] = "";
    Panel panel;
    ID3D12Resource* upload = nullptr;
    ID3D12Device* owner = nullptr;      // compared only, never used
    UINT texW = 0, texH = 0, fontPx = 0;
    std::vector<std::pair<ID3D12Resource*, int>> retired;
    void render(std::vector<uint32_t>& pixels, const char* status);
    void adjust(int row, int dir);
};

} // namespace d5
