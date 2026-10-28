# METROTONOPITECUS

Welcome to my first project for the Cardputer ADV. It is also a development of the software that will allow me later on to build my own metronomes based on the ESP32-S3. So, in theory, it will be just "copy and paste" and solder the components separately. It is already going to be more complicated than that because for sure I will upgrade the microphone to get a better accuracy on the lower and higher part of the register.

---

## Hardware

Right now, this is firmware for the Cardputer ADV, which you can buy. You can either compile it from this repository, or just download the firmware binary. Later on, I will publish it on the M5Launcher, so you can download it directly from there, but that is still in progress.

The idea is that I will create a target option that can use any ESP32-S3 microcontroller with your own buttons and your own screen. But right now, just buy a Cardputer ADV and flash it.

---

## The Metronome

It includes a tap beat mode, from 20 to 300bpm (I would say that is more than enough) and some subdivisions. For the pitch of the beep sound, I was starting to get too creative and after sharing it with some friends, I went back to just one pitch but different volumes. It is probably less disturbing.

For now the metronome I would say that it will stay like this, let's see after a couple of months of use. I find the LED and the volume way more clear and loud than my digital metronomes, so I hope it helps, especially for chamber music rehearsals. We all have been in the uncomfortable rehearsal playing piano to be able to hear the beep.


---

## The Tuner

At first I was just building a classical tuner with a drone function, since it was all I have used and known. But in the middle of the process, I had a viola lesson where my teacher showed me the Tunemap app, just to prove his point about my "creative" intonation. And it was just too good to obvious. Of course the screen of the Cardputer ADV is a bit too small, but I will try to build the second screen mod and anyway on the next step of the road map, building my own metronomes with separate components, the screen won't be an issue anymore. Anyway, give it a try, because after the second use, you just get used to reading the colours to see your tendencies, and because the cells are always starting from C, you will know where you are. It is a very tough exercise to practice a slow scale and see the screen becoming a Christmas tree with all the lights in different colours on. But a very good exercise I would say.


---

## A game inside a metronome!?

Yes, why not? It is a no-brainer way to disconnect from practice. I remember in my bachelor's days, before the Instagram era, with my 2nd generation iPod touch, the amount of breaks that I just played 5 min of Angry Birds, small siesta on the couch and then keep going with the practice. 

Even nowadays, I find myself that the best break from practice, photo editing or computer projects is some quick Mario Karts. Well, even better is just doing absolutely nothing, but we need to amuse ourselves from time to time. So better a simple game on the metronome, than touching the diabolical gadget with our entire lives that we call smartphone. 

Have fun!

---
## Special thanks

Many thanks to the folks of https://tonemap.live for sharing their code on GitHub https://github.com/crossrhythm/tonemap.live that allowed me to incorporate their incredible function into my design. You will find it by pressing the Tuner key (T) twice.

It is the same logic regarding, but translated to a different programming language, since the original code is written in HTML. For now all the basic options + transposing notes + a second palette of colours (a bit more radical and not made for colour-blind people) are included.

Next on the roadmap is to also adopt the different temperaments, that could be very useful for baroque and curious people.
