# sysHaze

Haze homebrew edited to run as sysmodule. Just plug USB and it's connected as MTP.

Original homebrew source code:<br>
https://github.com/Atmosphere-NX/Atmosphere/tree/master/troposphere/haze

Requires 4MB of RAM to work.

You can turn it off and on with Sysmodules overlay.<br>
https://github.com/ppkantorski/ovl-sysmodules/releases

If you want to add custom paths as partitions, create `config.ini` in `sdmc:/config/sysHaze/` and write there
```ini
[Name]
Path=FolderPath
```

f.e.
```ini
[FPSLocker]
Path=SaltySD/plugins/FPSLocker/patches
```
