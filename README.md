<img width="670" height="308" alt="image" src="https://github.com/user-attachments/assets/7a307e4c-4a11-4741-baf1-f883fcab75d8" /># MWVR-Screen-export
A tool for exporting Mechwarrior VR's HUD screens to a MFD or second monitor



https://github.com/user-attachments/assets/c656b5cd-2815-48f7-a1f9-5116d39b3c83


https://github.com/user-attachments/assets/6366acce-2a73-40ca-b0c1-bdcb68f87070

[Longer preview](https://youtu.be/vPZLwHl5C3E)

This tool adds changes to the original Mechwarrior VR mod by Sicsix by intercepting his custom rendering pipeline, duplicating the widgets and exporting it to a custom app (Main.cpp) to display. The code is there if you want to see it (entirely done in Claude, I'm not a programmer). 

**WARNING: This will overwrite HUD.dll in MechwarriorVR, it is working for me on flatscreen but I don't know what effects this will have on the VR experience. The original files are publicly available anyway so you can always just reinstall, but please be aware that I am not responsible for your MWVR breaking - reinstall if you encounter any issues and wish to revert**

**Setup instructions**
1. Install [Mechwarrior VR](https://www.nexusmods.com/mechwarrior5mercenaries/mods/1009) and follow ALL steps, ensure everything is working properly before you attempt to proceed any further.
2. Download the release files, there will be a HUD.dll file and HUDDisplay.exe file.
3. Navigate to the Mechwarrior-Win64-Shipping.zip (you should know where it is if you followed all the MWVR steps) <img width="682" height="322" alt="image" src="https://github.com/user-attachments/assets/a646a28e-f878-443f-b41a-de6a3d842bc0" />
4. Open the .zip file using whatever extraction program you have (I still use winrar lol...)<img width="732" height="441" alt="image" src="https://github.com/user-attachments/assets/323d269e-9648-489c-94b7-0b5b32aa518a" />
5. Open the plugins folder, it should look something like this <img width="736" height="453" alt="image" src="https://github.com/user-attachments/assets/fa563a37-6e8c-4046-a825-5b5127bfb0e3" />
6. Drag our new copy of HUD.dll into this folder and overwrite the original HUD.dll file
7. Import the plugins folder in UEVR <img width="1560" height="541" alt="image" src="https://github.com/user-attachments/assets/81a6f225-c038-48a0-9376-9f3f0c073ebe" />
8. Our new HUD.dll should now be the version that injects into the game along with the rest of the MWVR mod. This now means that our HUDDisplay.exe has a place to hook onto - run the game, inject UEVR and open HUDDisplay.exe (it will crash if being opened before UEVR is injected)
9. Once launched, it will take a few seconds to load up, then you'll see 2 screens, a log screen (which will be spamming "Widget 4 not active") and a black screen - minimize the log screen and drag the black screen to whereever you want the display to be showing
10. Hop into a mission, if everything is working, numpad 1-6 should show you each screen (The default screen is the target screen which will be blank until you target something). If you don't have a numpad you'll need to manually map a bind using autohotkey or something, you'll be rebinding these keys to your MFD anyway, sorry but I did not want to interfere with existing common keybinds. Numpad 7/8 will cycle forwards/backwards, Numpad 9 will be the zoom camera capture - this will require additional setup which I will detail in the next section. 
11. Remap your MFD buttons using a button remapper like Joystick Gremlin - if you have multiple screens/MFDs that you want to use, see the next section for further setup.

Congratulations you have set up the basics. If you want additional screens or to set up the zoom camera capture however, you have a bit more work to do (SKIP until step 4 if you don't care about the zoom camera):

1. You cannot simply run HUDDisplay.exe if you want the zoom camera capture, you will need to create a shortcut first <img width="418" height="543" alt="image" src="https://github.com/user-attachments/assets/8146b409-0d81-49a7-a5ef-9d62615bb1b5" />
2. Right click and go into properties <img width="836" height="511" alt="image" src="https://github.com/user-attachments/assets/d5ca715d-0e46-4320-9204-bf9b3aa3b3bf" />
3. You will need to specify the screen size for the zoom camera capture. This will take a fair bit of trial an error, the format is X and Y coordinate of where the capture begins, and then X and Y size of the capture. To do this, you need to add "screen Xstart Ystart Xend Yend" to the target name <img width="357" height="536" alt="image" src="https://github.com/user-attachments/assets/522c3de7-2474-41c7-8fe8-79db58ec6d4e" />
4. IF setting up multiple screens, you will need to create a new shortcut for each instance and add "ctrl", "alt" and "shift" in the target name as well. This will allow you to change displays on each window independently without needing to focus the window by pressing NumpadX, Ctrl+NumpadX, Alt+NumpadX or Shift+NumpadX (only 4 screens supported at the moment). You can use the same screen coordinates if you've already set them up previously. <img width="355" height="538" alt="image" src="https://github.com/user-attachments/assets/c7582a2a-0ccb-4397-8892-550af0737807" />
5. Remap all of these onto your MFD buttons using Joystick Gremlin or whatever your preference is.

Congratulations you're finally done. Enjoy the coolness, your hard work paid off hopefully.




