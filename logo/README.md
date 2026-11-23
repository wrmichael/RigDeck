# Start-up logos

The gold AC9HP brand logo shown on every unit for a few seconds at start-up (main/ui.cpp, showSplash).
About 100 kB as a 256-colour image. Font: Audiowide by
Astigmatic, SIL Open Font License 1.1 (OFL.txt).

    python3 gold_logo.py Audiowide-Regular.ttf AC9HP ac9hp.png 760 220
    python3 to_lvgl.py ../main/logo_img.c logo_ac9hp ac9hp.png
