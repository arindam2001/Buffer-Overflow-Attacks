#!/bin/bash

if ! command -v qemu-system-x86_64 > /dev/null; then
	echo "You do not have QEMU installed."
	echo "If you are on a Linux system, install QEMU and try again."
	echo "Otherwise, follow the lab instructions for your OS instead of using this script."
	exit
fi

# can we use the -nic option?
version=$(qemu-system-x86_64 --version \
	| grep 'QEMU emulator version' \
	| sed 's/QEMU emulator version \([0-9]\)\.\([0-9]\).*/\1.\2/')
major=$(echo "$version" | cut -d. -f1)
minor=$(echo "$version" | cut -d. -f2)

net=()
if (( major > 2 || major == 2 && minor >= 12 )); then
	net=("-nic" "user,ipv6=off,model=virtio,hostfwd=tcp:127.0.0.1:2222-:2222,hostfwd=tcp:127.0.0.1:8080-:8080,hostfwd=tcp:127.0.0.1:8888-:8888")
else
	net=("-netdev" "user,id=n1,ipv6=off,hostfwd=tcp:127.0.0.1:2222-:2222,hostfwd=tcp:127.0.0.1:8080-:8080,hostfwd=tcp:127.0.0.1:8888-:8888" "-device" "virtio-net,netdev=n1")
fi

case "$(uname -s)" in
    Darwin)
	ACCELERATOR="-machine accel=hvf"
	;;

    Linux)
	ACCELERATOR="-enable-kvm"
	;;

    *)
	ACCELERATOR=""
	;;
esac

qemu-system-x86_64 \
	-m 2048 \
	-nographic -serial mon:stdio \
	-cpu qemu64,+sse3,+ssse3,+sse4.1,+sse4.2 \
	"$@" \
	${ACCELERATOR} \
	"${net[@]}" \
	2402-SIL765.vmdk
