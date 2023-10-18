ARG PLATFORM=amd64
FROM ${PLATFORM}/alpine:3.10 AS build

WORKDIR /build

RUN apk add --no-cache wget cmake make gcc g++ linux-headers zlib-dev openssl-dev \
            automake autoconf libevent-dev ncurses-dev msgpack-c-dev libexecinfo-dev \
            ncurses-static libexecinfo-static libevent-static msgpack-c ncurses-libs \
            libevent libexecinfo openssl zlib

RUN set -ex; \
            mkdir -p /src/libssh/build; \
            cd /src; \
            wget -O libssh.tar.xz https://www.libssh.org/files/0.10/libssh-0.10.5.tar.xz; \
            tar -xf libssh.tar.xz -C /src/libssh --strip-components=1;
            
RUN set -ex; \
            cd /src/libssh; \
            wget http://archive.ubuntu.com/ubuntu/pool/main/libs/libssh/libssh_0.10.5-3ubuntu1.debian.tar.xz; \
            tar -xf libssh_0.10.5-3ubuntu1.debian.tar.xz; \
            find debian -name '*.patch' -exec patch -p1 -i {} \; 

RUN set -ex; \
            cd /src/libssh/build; \
            cmake -DCMAKE_INSTALL_PREFIX:PATH=/usr \
            -DBUILD_SHARED_LIBS=OFF ..; \
            make -j $(nproc); \
            make install

COPY compat ./compat
COPY *.c *.h autogen.sh Makefile.am configure.ac ./

ENV PKG_CONFIG_PATH /usr/lib64/pkgconfig:/usr/lib/pkgconfig
RUN ./autogen.sh && ./configure --enable-static
RUN make -j $(nproc)
RUN objcopy --only-keep-debug tmate tmate.symbols && chmod -x tmate.symbols
RUN ./tmate -V

FROM alpine:3.9

RUN apk --no-cache add bash
RUN mkdir /build
ENV PATH=/build:$PATH
COPY --from=build /build/tmate.symbols /build
COPY --from=build /build/tmate /build
