import QtQuick 2.12
import QtQuick.VirtualKeyboard 2.12

// Embedding InputPanel here at all - regardless of whether this item is
// currently visible - is what makes Qt Virtual Keyboard suppress its own
// automatic fullscreen-ish desktop panel and hand control to the app
// instead (its documented "application integration" mode).
//
// width is set explicitly (not bound to a parent - this is the QML
// document's root item, it has none) so the keyboard renders at a fixed,
// deliberately-smaller-than-fullscreen size instead of the style's own
// apparent default of assuming it owns the whole screen (confirmed live -
// leaving width unconstrained rendered nearly edge-to-edge). height is left
// unset on purpose: the style computes its own correct height from a given
// width (stacking rows of proportionally-sized keys), and forcing a guessed
// height here is what clipped the top row live earlier - let it size
// itself and have FloatingKeyboard's C++ side follow that real size
// (QQuickWidget's default SizeViewToRootObject mode) rather than the other
// way around.
InputPanel {
    id: inputPanel
    width: 1100
}
