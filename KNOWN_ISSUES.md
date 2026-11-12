# Known Issues (v0.1.0-alpha.1)

- **Version support**: Only Super Mario Galaxy Switch **1.0.0** (`010099C022B96000`) is supported. Super Mario Galaxy 2 is not yet included.
- **Split-screen camera collision**: Player 2's follow camera does not perform full level collision avoidance, so scenery can occasionally sit between the camera and Luigi.
- **Cutscene transitions**: Certain event cameras merge temporarily to keep cutscenes centered; Luigi's input is held neutral during those merges.
- **Performance**: Drawing the scene twice in split-screen increases GPU/CPU load; minor frame-rate dips below 60 FPS can occur in heavy areas.
- **Save backups**: This is an alpha mod. Always back up your save data before testing.
