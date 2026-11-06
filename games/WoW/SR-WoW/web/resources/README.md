None of the PNG images in this folder is committed: they are the game's, and git ignores them (the .gitignore of the repository).

The shell's theme (the images named Shell*.png: the dialog frame as a 3x3 picture for CSS border-image, its background tile, the tooltip frame, the red button in
its up, down and disabled states, and ShellStatusBar.png, the connection quality bar) is made by the build from the game installation it is built for
(`scons game=PATH`, default `data/`): `tools/shell_art.py` reads the textures from the archives in the game's order (Interface\DialogFrame\UI-DialogBox-Border
and -Background, Interface\Tooltips\UI-Tooltip-Border, Interface\BUTTONS\UI-Panel-Button-Up/Down/Disabled, Interface\TargetingFrame\UI-StatusBar), decodes
them and writes the PNG here. Only the images that are missing are made: one that is already here, whoever put it here, is kept, so a chosen or edited image
survives the build; delete it to have it made again (or run `python3 tools/shell_art.py --game PATH --overwrite`). web/style.css uses them; the status
bar is tinted green by the CSS (rgb(0, 200, 0) under an overlay blend) for the fill, on a dark green (rgb(0, 80, 0)) background, inside the frame of the
game's tooltips (ShellTooltipFrame.png).

The streaming indicator of World of Warcraft 5.x (Stream{Background,Circle,Frame,Spark}.png, DXT5 textures of 64x64 decoded to PNG) is not made by the
build: its textures (Interface\COMMON\Stream*.blp) are in the interface archive of a later client (build 15890), which the build does not have. A developer
who has them puts the four PNG files here; a page built without them shows the indicator without its images. web/prerun.js (Module.wowStreamIcon) puts
them together as MoP's Interface\FrameXML\StreamingFrame.xml does, over the canvas, while the game waits for data.
