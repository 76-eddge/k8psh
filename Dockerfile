
# Build using Alpine Linux
FROM alpine AS build

RUN apk add cmake g++ ninja
COPY . /root/k8psh/
WORKDIR /root/k8psh

RUN cmake -B build -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX=/k8psh-install -G Ninja .
RUN cmake --build build --target check install/strip

# Build a minimal image
FROM scratch AS deploy

COPY --from=build /k8psh-install/ /

WORKDIR /root
RUN ["/bin/k8pshd", "--version"]

ENTRYPOINT ["/bin/k8pshd"]
CMD ["--version"]
