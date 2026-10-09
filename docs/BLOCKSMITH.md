# Blocksmith

Blocksmith is block coding for LiteKern X. You build a program by snapping blocks together, then press **Run** and it happens on the **stage**, a 320 × 240 picture on the right. It's the native version of the Blocksmith web app: the web app's blocks write HTML, CSS and JavaScript for a browser. LiteKern X has no browser, so these blocks run directly instead.

## Using it
- **Add a block:** pick a category at the top left, then drag a block into the middle (the workspace).
- **Snap blocks together:** drop a block just under another one, or inside a `repeat`/`if`/`forever`. A line shows where it will go. Values (the round and pointed blocks) go into the white spaces in other blocks, which get an outline when you're close.
- **Move a block:** dragging it takes it and everything under it. Dragging a value pulls it out of its space.
- **Delete a block:** drag it back onto the palette.
- **Change a value:** click a white space and type, then press Enter (Esc puts it back). Click a menu (a darker space with an arrow) to choose a variable, a key or a colour.
- **Look around:** drag the workspace's background. Drag the palette's background to see more of a category.
- **Run:** the **Run** button, F5 or Ctrl+Enter starts every `when Run clicked` script. Clicking a script runs just that one. **Stop** or Esc stops everything.
- **Save:** the **Save** button or Ctrl+S. Projects are saved as `/Blocksmith/<name>.blocks` on the USB stick. Once a project has a name, Run and closing the window save it too. **Open** lists your projects; **New** starts an empty one.

## The blocks
Numbers are whole numbers (LiteKern X apps have no floating point), so `7 ÷ 2` is `3`. The stage's (0, 0) is its top left corner, with x going right up to 320 and y going down to 240, the same as a web page's canvas.

| Category | Blocks |
|---|---|
| Events | `when Run clicked`, `when [key] key pressed`, `when stage clicked`. Key and click scripts work until you press Stop |
| Control | `wait (ms)`, `repeat (n)`, `forever`, `if <> then`, `if <> then / else`, `repeat until <>`, `stop all` |
| Stage | `fill stage [colour]`, `pen colour [colour]`, `pen rgb (r) (g) (b)`, `circle x () y () size ()` (size is the radius), `box x () y () w () h ()`, `line () () to () ()`, `write [text] x () y ()`, `print [text]` (to the console) |
| Maths | `+`, `-`, `×`, `÷`, `mod`, `random () to ()`, `<`, `=`, `>`, `and`, `or`, `not`, `join [] []` |
| Variables | **New variable**, `set [var] to []`, `change [var] by ()`, and a block for each variable |
| Sensing | `mouse x`, `mouse y`, `mouse down?`, `key [key] pressed?`, `timer` (ms since Run) |

Loops pause for a moment at the end of each time round once something has been drawn, as in Scratch, so `forever` with drawing in it makes an animation (about 30 frames a second) instead of a blur. If something goes wrong, for example a division by zero, the console says what happened in red, the block that failed gets a red outline, and everything stops.

## Limits
- Up to 480 blocks and 12 variables in a project. Text values are up to 63 characters, and what you type into a block up to 23.
- No sprites, sounds, lists or your own blocks yet. No undo: if you drop a block onto the palette by mistake, it's gone.
- A project is a short text file (format in `apps/blocksmith/project.c`). The Files app can rename or delete one.

## For developers
`apps/blocksmith/`: `blocks.c` (what each block is, layout, drawing, snapping), `run.c` (the interpreter: each running script is a thread with its own stack of loops, and they take turns on a 33 ms timer), `project.c` (files and the starter project) and `main.c` (the window). To add a block, add it to `enum op` in `blocksmith.h` and to `ops[]` in `blocks.c` (its name in files must never change), then to `step()` or `eval()` in `run.c`.
