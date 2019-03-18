# Share Android 8.0 network to Mac High Sierra 10.13.3 by horndis
I can confirm Horndis works with Honor V10 on High Sierra 10.13.3. Here's a quick guide:

1. brew cask install horndis
1. You might have to open System Preferences -> Security & Privacy -> General and whitelist the installer to run.
1. Reboot OSX, press and hold CMD+R until Recovery Mode boots, then open Terminal. 

		reboot 
	
  run the command:
  
		csrutil disable

1. Restart and run:

		sudo kextload /System/Library/Extensions/HoRNDIS.kext
1. Successful connection is shown up in my System Preferences -> Network list.
![image/HoRNDIS.png](image/HoRNDIS.png)

## Reference:
[1] https://github.com/jwise/HoRNDIS/issues/72