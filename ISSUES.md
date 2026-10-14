[ ] - Ongoing issue
[X] - Resolved issue
[?] - Issue needs feedback
[/] - Partially resolved

Issues:

[X] Enemies need to also mask their reflection the same way the hero does on water tiles, so that water will always have a reflection, but grass will not.
    Resolved: enemy/NPC reflections run through the same water-tile mask
    as the hero's, and the gate now accepts deep water too (the zora
    stands in it) — water tiles always mirror what stands in them, grass
    never shows a reflection.
[X] Items and such need to always be on top of the hero sprite, so that if it's unable to be collected, it doesn't look like a background tile.
    Resolved: pickups draw after the y-sorted entity pass (drawPickups in
    src/main.cpp), above the hero — an uncollectible heart still reads as
    an item in the room.
[X] Game-Code Behaviors need to be editable within the Resource Editor. Give me a code editor in the window.
    Resolved: a Code tab — file list with scaffold/delete for
    resources/code/*.cpp, a monospace editor (tab key, dirty guard) with
    Save and Compile check. The backend save command accepts .cpp only
    under resources/code/, everything else unchanged.
[X] Dialogue choice arrows seem to overlap with text, which is not ideal.
    Resolved: the selection triangle drew 14px inside the right-aligned
    option text; it now points at the selected option from its left side,
    LttP style.
[X] My spritesheet might not support it, but Characters need to have spots for more than just "stand" and "move". I may just duplicate frames for the time being.
    Resolved: characters gain four optional state channels — attack
    (swing/spin body), lift (hoisting + carrying), swim, hurt — same
    {down/right/left/up: [frames]} shape as stand/move. Empty channels
    fall back to stand/move at runtime (duplicate stand frames into a
    channel for placeholder art, as planned). The character editor's
    Animation picker and the new-character scaffold include the channels.
[X] Make sure that scrolling works and makes sense across the Resource Editor, such as the map editor and side panels.
    Resolved: the app is a fixed-viewport tool window — the tab nav stays
    put and main owns the page scroll; the room editor's side panel sticks
    beside the map and scrolls on its own; the world graph and dialogue
    trees scroll inside a full-height surface instead of growing the page;
    the character sheet's side panel follows the sheet; ctrl/cmd+wheel
    zooms the map canvas.
