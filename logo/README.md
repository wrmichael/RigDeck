# Start-up screen

The start-up screen shows the station's callsign (Settings) large in polished gold, with "RigDeck by
AC9HP" under it. The gold is computed on the board from the letter shapes (main/ui.cpp, goldText):
soft bevel lit from the upper left, highlight, metal bands, brushed texture, dark rim, warm shadow.

Font: Audiowide by Astigmatic, SIL Open Font License 1.1 (OFL.txt). The board carries only the
characters a callsign needs (main/logo_font.c, about 5 kB), made with:

    pyftsubset Audiowide-Regular.ttf --unicodes="U+0020,U+002F,U+0030-0039,U+0041-005A" \
        --output-file=audiowide_call.ttf --no-hinting --desubroutinize --drop-tables+=GPOS,GSUB,kern

gold_logo.py renders the same look on a PC (PNG preview): `python3 gold_logo.py Audiowide-Regular.ttf AC9HP ac9hp.png 760 220`.
