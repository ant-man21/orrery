notes
I remade image and now instead of it going to menu it boots immediately in to shell (fine but why)
???

how does the TPM setup work???
- uses swtpm socket command but what does it do?
- then a flag is used in qemu....?

Sync works. what was I doing new now? how does it work?
- build makes the efis and stiches into the .FD (found in x64 folder)
- then the build finishes by parse .sync list and putting it into the shared folder in Q35Pkg/
- the runner script qemu.sh, builds the shared.img somehow
    -uses dd, mformat, mcopy, mkfs.fat commands...
- then use -drive flag to load it as a FAT formatted storage device?

We created first part of app but what does it actually do? TCG2 protocol is not found. why?
- (I set a flag -D TPM2_ENABLE=TRUE)
- I was able to fix the tpm driver to start running properly and finds TPM2 device but it gets an runtime exception
- I added debug but I dont see it in the log. I could use prints but why does DEBUG macro not work??
- App does a few things (ignoring commented out parts)
    - it first just locatge Tcg2Protocol
    - then it prints the Tcg2Caps? idk waht that means
        - basically shows the tpm capabilities?
    - then we read rom image and save it to memory
        - somehow the ai knew exactly where in memory it would be... but how
    - then we attempt to extend PCR16 
        - is a PCR a slot? like a mem slot? or reg?
        - we are attempting it looks like we hash but dont finish this for some reason
    - then we would try and save file to disk to use as a test later