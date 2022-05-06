
# Build using Alpine Linux
FROM alpine AS build

RUN apk add git cmake g++ ninja
COPY . /root/k8psh/
WORKDIR /root/k8psh

RUN cmake -B build -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX=/k8psh-install -G Ninja .
RUN cmake --build build --target check install/strip

RUN cmake -B build_debug -DCMAKE_BUILD_TYPE=MinSizeRel '-DCMAKE_CXX_FLAGS=-Os -DDEBUG' -DCMAKE_INSTALL_PREFIX=/k8psh-debug-install -G Ninja .
RUN cmake --build build_debug --target check install/strip

# Build a minimal image
FROM scratch AS deploy

COPY --from=build /k8psh-install/ /

WORKDIR /root

ENTRYPOINT ["/bin/k8psh"]
CMD ["--version"]

# Build a minimal debug image
FROM scratch AS deploy-debug

COPY --from=build /k8psh-debug-install/ /

WORKDIR /root
ENV K8PSH_DEBUG=all

ENTRYPOINT ["/bin/k8psh"]
CMD ["--version"]
