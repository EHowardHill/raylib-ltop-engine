#!/usr/bin/env python3
"""Dialogue scripts for ENOKI AND THE HOLLOW KING: every line of text the
game speaks, written to resources/dialogue/*.json.

Vocabulary (engine-interpreted): text, label, if/unless (flags),
if_item/unless_item, set/clear, sfx, music, give, goto, warp, choice
(text/goto/set/clear/sfx/cost/give), wait, camera, letterbox, move, face,
show/hide, fade, image; dialogue-level credits and next.
"""

import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "resources", "dialogue")


def L(text=None, **kw):
    line = {}
    if text is not None:
        line["text"] = text
    if "if_" in kw:
        kw["if"] = kw.pop("if_")
    if "unless_" in kw:
        kw["unless"] = kw.pop("unless_")
    line.update(kw)
    return line


DIALOGUES = {}

# --------------------------------------------------------------------------
# The boot intro: played over the village drop on a fresh game (the
# intro_seen flag banks automatically, so it never replays).

DIALOGUES["intro"] = {
    "lines": [
        # The sleeping heroine is hidden while the camera finds the storm.
        L(letterbox=True, hide="hero",
          camera={"pan": [12, 6], "time": 2.4}, wait=2.6),
        L(text="The night the storm came to Willowdown, something "
               "crept out of the Sunken Crypt...",
          sfx="thunder.wav", wait=3.4),
        L(fade={"to": "black", "time": 0.8}, wait=1.0),
        L(image="intro_card.png", letterbox=True,
          text="It took the HEARTSTONE, and the sky has not "
               "stopped screaming since.", sfx="rain.wav", wait=3.6),
        L(image="", fade={"to": "world", "time": 0.8},
          camera="hero", wait=0.9),
        # She wakes: back on stage, on her feet, and Aaron is shouting.
        L(show="hero", face={"actor": "hero", "dir": "down"},
          text="...enoki... ENOKI! Wake up, the Elder is asking "
               "for you!", sfx="message.wav", wait=2.4),
        L(letterbox=False, camera="hero", wait=0.4),
        L(text="Rain in the night, rain in the morning. "
               "Go and see Aaron first - he's worried sick.",
          sfx="message_finish.wav"),
    ]
}

# --------------------------------------------------------------------------
# Aaron: the oldest friend, one stage per story beat.

DIALOGUES["aaron"] = {
    "lines": [
        # Starts with nothing: no blade, storm on the fields.
        L(text="Enoki! You're awake! Thank the fields.",
          unless_item="sword"),
        L(text="The Heartstone is gone and the storm won't quit. "
               "Elder Rowan is asking for you.", unless_item="sword"),
        L(text="His house is the one with the lantern - go on, "
               "I'll watch the road.", unless_item="sword",
          set=["aaron_sent"]),
        # Sword in hand: the fields are open.
        L(text="That's Rowan's old blade! Heh, look at you.",
          if_item="sword", unless_="entered_crypt"),
        L(text="The meadow north of here hides bombs in the "
               "grass caves. You'll want those.", if_item="sword",
          unless_="entered_crypt"),
        L(text="And watch the ridge - the storm up there bites. "
               "The Crypt's old stair is the only way in.",
          if_item="sword", unless_="entered_crypt"),
        # In the crypt: practical hints.
        L(text="You found the Crypt's stair? Don't go dying "
               "down there, you hear?", if_="entered_crypt",
          unless_="game_complete"),
        L(text="Mara restocks her stall if you need anything. "
               "Fen keeps an eye out for bees.", if_="entered_crypt",
          unless_="game_complete"),
        # Post-game.
        L(text="Look at that - the sky's letting up! You actually "
               "did it, Enoki.", if_="game_complete"),
        L(text="The Heartstone hums in the shrine like it never "
               "left. Village owes you a festival.", if_="game_complete"),
        L(text="Rain's done! Done! I'm going puddle-jumping!",
          if_="game_complete", sfx="cucco.wav"),
    ]
}

# --------------------------------------------------------------------------
# Elder Rowan: the retired adventurer in his house.

DIALOGUES["elder"] = {
    "lines": [
        L(text="So. The rain woke you, and Aaron woke your "
               "courage.", unless_item="sword"),
        L(text="Listen well: the Hollow King took the Heartstone "
               "below the ridge, into the Sunken Crypt.",
          unless_item="sword"),
        L(text="My adventuring days are done. Take my old blade "
               "from the chest.", unless_item="sword"),
        L(text="Press X to swing it. Hold X to charge a spin. "
               "Z lifts things. ENTER rests and saves.",
          unless_item="sword"),
        # Post-sword.
        L(text="That blade's yours now. A sword's a promise - "
               "keep it sharp.", if_item="sword",
          unless_="entered_crypt"),
        L(text="The Crypt's stair waits on the ridge north of "
               "the meadow. Bomb the cracked walls.",
          if_item="sword", unless_="entered_crypt"),
        L(text="Green dust wakes the sleeping. It matters "
               "below - trust an old man.", if_item="sword",
          unless_="entered_crypt"),
        # Crypt era.
        L(text="The old signs down there still give true hints. "
               "Read them all.", if_="entered_crypt",
          unless_="game_complete"),
        L(text="If you meet a wizard who laughs at your sword... "
               "hit him with his own magic.", if_="entered_crypt",
          unless_="game_complete"),
        # Post-game.
        L(text="The Heartstone hums again, and the rain's gone. "
               "You've repaid us all, child.", if_="game_complete"),
    ]
}

# --------------------------------------------------------------------------
# Mara's shop stall.

DIALOGUES["mara"] = {
    "lines": [
        L(text="Mara's goods! Storm prices! What'll it be?",
          sfx="rupee.wav"),
        L(text="Take your pick.",
          choice=[
              {"text": "BOOMERANG - 30 RUPEES", "cost": 30,
               "give": "boomerang", "sfx": "wallet 1.wav",
               "goto": "thanks", "set": ["shop_served"]},
              {"text": "NOT TODAY"},
          ]),
        L(label="thanks",
          text="A fine pick! It always comes back to you. "
               "Eventually. Usually."),
        # Post-purchase flavor.
        L(text="Tell old Rowan he still owes me for rope.",
          if_="shop_served", unless_="game_complete"),
        L(text="Storm's broke! Everything must go! Well - "
               "everything on this shelf.", if_="game_complete"),
    ]
}

# --------------------------------------------------------------------------
# Fen: the kid with the bee stories.

DIALOGUES["fen"] = {
    "lines": [
        L(text="Enoki! The bees in the grove took my whistle! "
               "Fact!", unless_="game_complete"),
        L(text="Grampa says the shrine in the grove's got a "
               "sleepy statue in it. Fact!",
          unless_="game_complete"),
        L(text="You caught a bee?! Bottle it! FACT!",
          if_item="net", unless_="game_complete"),
        L(text="The rain stopped 'cause of YOU? Okay. "
               "OKAY! FINE! Fact.", if_="game_complete",
          sfx="heart.wav"),
    ]
}

# --------------------------------------------------------------------------
# The Grove Keeper: sleeps until magic powder wakes the shrine.

DIALOGUES["keeper_wakes"] = {
    "lines": [
        # The shrine's awakening moment (played by the sprinkle itself).
        L(text="The sleeping stone yawns. Dust of green dreams "
               "settles into its cracks.", sfx="prayer 2.wav", wait=1.8),
        L(text="Something under the moss stretches... sits up... "
               "and looks around for you."),
        L(text="He will wait by the path south of the shrine. "
               "Go and meet him.", sfx="message_finish.wav"),
    ]
}

DIALOGUES["keeper"] = {
    "lines": [
        L(text="Ahh... the dust of green dreams. Long years, "
               "little forager.", sfx="prayer 1.wav",
          face={"actor": "hero", "dir": "up"}),
        # The keeper steps down off the shrine stones to speak.
        L(move={"actor": 0, "tile": [12, 5], "time": 1.4}, wait=1.6),
        L(text="You walk toward the Hollow King. You will need "
               "to walk where the sun is thinner.",
          face={"actor": 0, "dir": "down"}),
        L(text="Take my glass. It steps between the world and "
               "its shadow.",
          sfx="secret.wav", give="mirror"),
        L(text="Use it in the grove: the Hollow mirrors this "
               "place. What is lost here sleeps there."),
        # Post-mirror.
        L(text="The Hollow is thick with his soldiers. The cape "
               "and the gloves you need are there.",
          if_="mirror", unless_="game_complete"),
        L(text="The grove sings again! Go home, child. "
               "The rain has.", if_="game_complete"),
    ]
}

# --------------------------------------------------------------------------
# Signs.

DIALOGUES["sign_village"] = {
    "lines": [
        L(text="WILLOWDOWN VILLAGE. \"Small in the storm, "
               "stout in the sun.\""),
        L(text="North: the meadow and the ridge. East: "
               "Elder's Grove. South: Long Shore."),
    ]
}

DIALOGUES["sign_controls"] = {
    "lines": [
        L(text="ARROWS walk. Z lifts, reads, opens. X swings "
               "the sword - hold it to spin!"),
        L(text="C uses your equipped item, V dashes with boots, "
               "and SHIFT guards."),
        L(text="ENTER rests. The valley keeps your journey "
               "every time you cross a door."),
    ]
}

DIALOGUES["sign_meadow"] = {
    "lines": [
        L(text="ELDER'S FIELD. Cut the tall grass - "
               "the valley hides gifts in it."),
        L(text="Beware soldiers on patrol. They charge "
               "when they see red."),
    ]
}

DIALOGUES["sign_grove"] = {
    "lines": [
        L(text="ELDER'S GROVE. Mind the bees, mind the pond, "
               "mind the sleeping stone."),
        L(text="Do NOT wake the shrine without green dust. "
               "It is terribly rude."),
    ]
}

DIALOGUES["sign_legend"] = {
    "lines": [
        L(text="The shrine of the grove. Below, the legend of "
               "the valley...", sfx="secret.wav"),
        L(image="legend.png", letterbox=True,
          text="THE LEGEND OF THE VALLEY",
          sfx="prayer 2.wav", wait=2.8),
        L(image="", letterbox=False,
          text="When the Heartstone sleeps at home, the valley "
               "keeps the sun.", sfx="message_finish.wav"),
    ]
}

DIALOGUES["sign_shore"] = {
    "lines": [
        L(text="LONG SHORE. The current runs strong along "
               "the shallows."),
        L(text="Swimmers need flippers. Everyone else needs "
               "sense."),
    ]
}

DIALOGUES["sign_ridge"] = {
    "lines": [
        L(text="RIDGEWAY. The storm breaks hardest here."),
        L(text="The Sunken Crypt lies below. Its stair is old "
               "and its doors are worse."),
    ]
}

DIALOGUES["sign_crypt"] = {
    "lines": [
        L(text="THE SUNKEN CRYPT. Trespassers will be "
               "drowned, spiked, bitten, or promoted."),
        L(text="East wing: the crystal is honest but its "
               "barriers are not."),
        L(text="West wing: bring a lantern, a key, and "
               "patience."),
    ]
}

DIALOGUES["sign_east"] = {
    "lines": [
        L(text="FROST WING. The floor is a liar. "
               "Stand still and it tells on you."),
        L(text="Strike the crystal to trade which barriers stand. "
               "The blue ones stand down; the red, shut."),
    ]
}

DIALOGUES["sign_west"] = {
    "lines": [
        L(text="DARK WING. Light your own way."),
        L(text="Blocks sit hard on switches. Pull the lever "
               "for the old toll gate."),
    ]
}

DIALOGUES["sign_pit"] = {
    "lines": [
        L(text="THE MIRE. The floor moves where it wants. "
               "Ride the raft."),
        L(text="What falls through lands below. "
               "This is a promise, not a warning."),
    ]
}

DIALOGUES["sign_lower"] = {
    "lines": [
        L(text="THE CISTERN. Green dust wakes sleepers. "
               "Red blocks obey the cane."),
        L(text="A conjured block weighs as much as "
               "an honest one."),
    ]
}

DIALOGUES["sign_sanctum"] = {
    "lines": [
        L(text="THE SANCTUM. Beyond waits the throne "
               "of the Hollow King."),
        L(text="Bats answer laughter. Remember it."),
    ]
}

# --------------------------------------------------------------------------
# One-shot beats (encounters).

DIALOGUES["beat_meadow"] = {
    "lines": [
        L(text="Rain over Elder's Field... Somewhere under this "
               "grass there's a bomb bag with your name on it."),
    ]
}

DIALOGUES["beat_grove"] = {
    "lines": [
        L(text="Fog pools between the trees. This grove is old "
               "and it is listening."),
    ]
}

DIALOGUES["beat_shore"] = {
    "lines": [
        L(text="Something is watching from the deep water. "
               "It spits first and apologizes never."),
    ]
}

DIALOGUES["beat_ridge"] = {
    "lines": [
        L(text="The storm screams up here. The crypt stair is "
               "close - so is the lightning.", sfx="thunder.wav"),
    ]
}

DIALOGUES["beat_hollow"] = {
    "lines": [
        L(text="The Hollow. The same grove, wronged - bent "
               "grass, thin light, soldiers with hollow eyes.",
          set=["hollow_seen"]),
    ]
}

DIALOGUES["sign_soil"] = {
    "lines": [
        L(text="SOFT SOIL. The field keeps its promises buried. "
               "Bring a shovel, keep your back straight."),
    ]
}

DIALOGUES["beat_crypt"] = {
    "lines": [
        L(text="The Sunken Crypt. The air tastes of "
               "drowned bells.", sfx="ganon's tower opens.wav",
          set=["entered_crypt"]),
    ]
}

DIALOGUES["beat_sanctum"] = {
    "lines": [
        L(text="Past the big door... something heavy breathes "
               "in armor."),
    ]
}

DIALOGUES["beat_throne"] = {
    "lines": [
        L(text="The throne room floods with pale light. "
               "The dais waits.", unless_="boss_defeated_18_crypt_throne"),
    ]
}

DIALOGUES["sign_shrine"] = {
    "lines": [
        L(text="THE SLEEPING KEEPER. Do not wake him "
               "without the dust of green dreams."),
        L(text="Green dust, sprinkled with respect. "
               "NEVER with a shovel."),
    ]
}

DIALOGUES["sign_throne"] = {
    "lines": [
        L(text="THE THRONE OF THE HOLLOW KING. "
               "No refunds, no appeals, no mercy."),
        L(text="His laughter bounces. So does his magic. "
               "Bats answer laughter."),
    ]
}

DIALOGUES["beat_village"] = {
    "lines": [
        L(text="The north gate creaks in the wind. "
               "Somewhere out there, thunder answers."),
    ]
}

DIALOGUES["peek_big_door"] = {
    "lines": [
        L(text="A great door bars the way north. Its keyhole "
               "gapes big enough to fall into."),
    ]
}

# --------------------------------------------------------------------------
# The ending: the Heartstone restored, the storm broken.

DIALOGUES["ending"] = {
    "lines": [
        L(text="The Hollow King comes apart like smoke in "
               "wind...", letterbox=True,
          face={"actor": "hero", "dir": "up"},
          sfx="boss dies.wav", wait=1.2),
        L(text="and the Heartstone rolls out of his crown, "
               "still humming.", wait=2.6),
        L(camera={"pan": [10, 6], "time": 2.0}, wait=2.2),
        L(text="Enoki lifts it, and the storm forgets "
               "why it was angry.", wait=2.6),
        L(fade={"to": "black", "time": 0.9}, hide="hero", wait=1.1),
        L(image="ending.png", letterbox=True,
          text="Over Willowdown, the clouds finally part.",
          sfx="life refill.wav", wait=3.4),
    ],
    "next": "credits.json",
}

DIALOGUES["credits"] = {
    "credits": True,
    "lines": [
        L(text="ENOKI AND THE HOLLOW KING", music="gerudo.xm"),
        L(text="A valley's tale on the LTOP engine"),
        L(text="ENOKI - the forager who went to the crypt"),
        L(text="AARON - the friend who watched the road"),
        L(text="ELDER ROWAN - the blade that came out of "
               "retirement"),
        L(text="MARA - storm prices, honest goods"),
        L(text="FEN - facts!"),
        L(text="THE GROVE KEEPER - dream dust, old deals"),
        L(text="THE HOLLOW KING - drowned crown, "
               "no refunds"),
        L(text="rooms, tiles, sprites, sounds - "
               "baked, not parsed"),
        L(text="THE END", wait=3.5, set=["game_complete"],
          show="hero",
          warp={"room": "01_village", "tile": [12, 11]}),
    ]
}


def main():
    os.makedirs(OUT, exist_ok=True)
    for old in sorted(os.listdir(OUT)):
        if old.endswith(".json"):
            os.remove(os.path.join(OUT, old))
            print("removed old dialogue %s" % old)
    for name, spec in sorted(DIALOGUES.items()):
        path = os.path.join(OUT, name + ".json")
        with open(path, "w") as handle:
            handle.write(json.dumps(spec, indent=4))
            handle.write("\n")
        print("wrote %s (%d lines)%s" % (
            os.path.relpath(path, ROOT), len(spec["lines"]),
            " [credits]" if spec.get("credits") else ""))
    print("%d dialogues" % len(DIALOGUES))


if __name__ == "__main__":
    main()