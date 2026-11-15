TEMPLATE = aux
TARGET = harbour-communi

SIZES = 86 108 128 172

# The PNG icons are pre-rendered from artwork/$${TARGET}.svg and kept in
# icons/, because the SVG converter (python3-cairosvg) isn't available in the
# Sailfish SDK repositories. To regenerate them after changing the SVG:
#   for s in $$SIZES; do rsvg-convert -w $s -h $s -o icons/$s"x"$s/$${TARGET}.png artwork/$${TARGET}.svg; done
for(size, SIZES) {
    icon$${size}x$${size}.files = $$PWD/icons/$${size}x$${size}/$${TARGET}.png
    icon$${size}x$${size}.path = /usr/share/icons/hicolor/$${size}x$${size}/apps
    INSTALLS += icon$${size}x$${size}
}

#icon_svg.files = $$PWD/artwork/$${TARGET}.svg
#icon_svg.path = /usr/share/icons/hicolor/scalable/apps
#INSTALLS += icon_svg

desktop.files = $$PWD/$${TARGET}.desktop
desktop.path = /usr/share/applications
INSTALLS += desktop
