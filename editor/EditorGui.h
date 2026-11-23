#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Layout constants ported from Quake/qr_gui.cpp for two-column QuakeRay inspector rows.
inline constexpr float kEditorGuiPanelWidth = 440.0f;
inline constexpr float kEditorGuiLabelWidth = 172.0f;
inline constexpr float kEditorGuiResetSize = 24.0f;
inline constexpr float kEditorGuiActionSize = 24.0f;
inline constexpr float kEditorGuiBrowseWidth = 26.0f;

// Per-row options shared by two-column inspector widgets to keep widget signatures concise.
struct EditorGuiRowOpts
{
    bool changed = false;
    bool enabled = true;
    const char *tooltip = nullptr;
    const char *format = nullptr;
    bool *resetClicked = nullptr;
    // Optional callback receiving screen-space rect ("x0,y0 x1,y1") for automated test hooks.
    std::function<void(const char *)> logRect;
    const char *rectName = nullptr;
    bool dimValue = false;
};

// Action flags returned by EditorGuiBlockActions / EditorGuiHeaderActions.
enum EditorGuiBlockAction : uint32_t
{
    kEditorGuiActionNone = 0,
    kEditorGuiActionCopy = 1 << 0,
    kEditorGuiActionPaste = 1 << 1,
    kEditorGuiActionRemove = 1 << 2,
};

// Callback interface for searchable filtered combos (label lookup + optional display badge).
struct EditorGuiComboModel
{
    int32_t count = 0;
    std::function<const char *(int32_t)> labelAt;
};

// Applies the dark translucent QuakeRay visual theme and scales metrics for HiDPI displays.
void EditorGuiApplyStyle(float displayWidth, float displayHeight);

// Returns the active UI scale factor computed by EditorGuiApplyStyle (1.0 at <=1080p, 1.5 at 4K).
float EditorGuiScale();

// Begins the right-docked inspector panel window with a styled header bar.
bool EditorGuiBeginPanel(const char *title, float displayWidth, float displayHeight);

// Ends the right-docked inspector panel window started by EditorGuiBeginPanel.
void EditorGuiEndPanel();

// Begins a scrollable region filling the remaining vertical space inside the panel.
void EditorGuiBeginScroll(const char *id);

// Ends the scrollable region started by EditorGuiBeginScroll.
void EditorGuiEndScroll();

// Draws equal-width tabs plus an optional trash reset button on the right edge.
// Returns true if the user clicked a different tab.
bool EditorGuiTabs(const char *const *labels, int32_t count, int32_t *activeIndex,
                   bool showResetTrash, bool *outResetClicked, const char *resetTooltip);

// Draws a tinted section header bar with separator line and optional hover tooltip.
void EditorGuiSectionHeader(const char *label, const char *tooltip = nullptr);

// Draws a vector-drawn circular reset-arrow button; interactive only when `changed` is true.
bool EditorGuiResetButton(const char *id, bool changed, const char *tooltip = nullptr);

// Draws Copy / Paste / Remove vector icon buttons right-aligned on the current line.
uint32_t EditorGuiBlockActions(const char *id, bool canCopy, bool canPaste, bool canRemove);

// Draws a two-column checkbox row with optional reset icon on the right.
bool EditorGuiCheckbox(const char *label, bool *value, const EditorGuiRowOpts &opts = {});

// Draws a two-column float slider row with optional reset icon on the right.
bool EditorGuiSliderFloat(const char *label, float *value, float minVal, float maxVal,
                          const EditorGuiRowOpts &opts = {});

// Draws a two-column integer slider row with optional reset icon on the right.
bool EditorGuiSliderInt(const char *label, int32_t *value, int32_t minVal, int32_t maxVal,
                        const EditorGuiRowOpts &opts = {});

// Draws a two-column fixed combo box row with optional reset icon on the right.
bool EditorGuiCombo(const char *label, int32_t *selected, const char *const *items,
                    int32_t itemCount, const EditorGuiRowOpts &opts = {});

// Draws a two-column searchable combo popup row backed by `model`.
bool EditorGuiComboFiltered(const char *label, int32_t *selected, const EditorGuiComboModel &model,
                            const EditorGuiRowOpts &opts = {});

// Draws a two-column texture path box with a `[...]` browse button and optional reset icon.
bool EditorGuiTexturePath(const char *label, std::string &path, bool *browseClicked,
                          const EditorGuiRowOpts &opts = {});

// Draws a two-column RGB swatch + `#RRGGBB` hex picker row with optional reset icon.
bool EditorGuiColorHex(const char *label, float rgb[3], const EditorGuiRowOpts &opts = {});

// Draws an inline compact RGB swatch + `#RRGGBB` hex editor (used by luma block rows).
bool EditorGuiColorRow(const char *id, float rgb[3], const EditorGuiRowOpts &opts = {});

// Draws a two-column X/Y/Z vector drag row with colored axis tags and optional reset icon.
bool EditorGuiVec3Input(const char *label, float vec[3], float step,
                        const EditorGuiRowOpts &opts = {});

// Draws a fullscreen dimmed modal backdrop behind centered dialogs.
void EditorGuiBackdrop(float displayWidth, float displayHeight, float alpha = 0.58f);

// Draws a centered modal dialog with horizontal action buttons; returns clicked index or -1.
int32_t EditorGuiDialogCentered(const char *id, const char *title, const char *message,
                                const char *const *buttons, int32_t buttonCount);

// Draws a centered modal chooser with vertical stacked action buttons and subtitles.
int32_t EditorGuiDialogVertical(const char *id, const char *title, const char *subtitle,
                                const char *const *buttons, const char *const *descriptions,
                                int32_t buttonCount);
