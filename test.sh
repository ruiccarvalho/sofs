#!/bin/bash
### compile
gcc -Wall sofs.c `pkg-config fuse --cflags --libs` -o sofs
gcc -Wall -o mkfs_sofs mkfs_sofs.c
mkdir mnt
./mkfs_sofs
./sofs abc mnt
cp sofs.c mnt
cp mnt/sofs.c ./sofscpd.c
diff -s sofs.c sofscpd.c
umount mnt
# test if file consistency is preserved after unmounting
./sofs abc mnt
rm sofscpd.c
cp mnt/sofs.c ./sofscpd.c
diff -s sofs.c sofscpd.c
for i in {1..20}
do
echo "Line $i has some text" >> text.txt
done
cp text.txt mnt
diff -s text.txt mnt/text.txt
umount mnt
