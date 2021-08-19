# Kubernetes Pod Shell (k8psh)
k8psh (/kə-ˈpēsh/) is a minimal shell allowing a Pod container to run an executable inside another container.

## Building
The project can be built using cmake:
```shell
$ cmake -B build -DCMAKE_BUILD_TYPE=MinSizeRel -G Ninja .
$ cmake --build build
```

For testing, use the "Debug" build type. Debugging output can be enabled by individual files (`K8PSH_DEBUG="Process, Main"`) or globally (`K8PSH_DEBUG=All`) by setting the environment variable `K8PSH_DEBUG`.
```shell
$ cmake -B build -DCMAKE_BUILD_TYPE=Debug -G Ninja .
$ cmake --build build --target check -v
```

The project is compiled into a statically-linked binary. This allows better portability between different operating systems and containers by eliminating dependencies on system libraries.

## Running
The project is divided into two components: a client, `k8psh`, and a server, `k8pshd`. Both components are contained in the same binary. `k8pshd` is typically hardlinked or symlinked to `k8psh`, and the name of the executable is used to determine whether to run in client or server mode. Running `k8pshd` is equivalent to running `k8psh --server`.

In practice, the client is only used with the `--install` or `--server` options. It is invoked implicitly by any stub executables, but this is not visible to the end user. It is also used for interactive testing with a running server, since the configuration file and client command can be changed on the command line.

A configuration file is used to configure the clients and servers. (See the example configuration file for more details.) It can be set on the command line with the `--config` option or using the environment variable `K8PSH_CONFIG`. Specifying the option will override any value in the environment variable.

## Installing
The project can be installed in multiple ways:
 1. Using the cmake `install` target.
 2. Copying the binaries into a directory in `$PATH`.
 3. Runing `k8psh --install [path/to/file]`.

The preferred way to install into a container is using the `--install` option. Configuring the k8psh container as a Pod init container allows `k8psh` to be installed into a shared volume before other containers run. (An alternative is to already have `k8psh` installed in a shared persistent volume.) Other containers then run the executable in the shared volume. An example is given in `test/k8s/test.yaml`. The relevant parts are shown below:
```yaml
spec:
  initContainers:
  - name: k8psh
    image: 76eddge/k8psh
    args: [ --install, /k8psh/bin/k8pshd ]
    volumeMounts:
    - mountPath: /k8psh/bin
      name: k8psh-bin
  containers:
  - name: server
    image: centos:6
    command: [ /bin/sh, -c, '/k8psh/bin/k8pshd --config=/workspace/test/config/k8s.conf --name=server' ]
    volumeMounts:
    - mountPath: /k8psh/bin
      name: k8psh-bin
      readOnly: true
  - name: client
    image: ubuntu
    tty: true
    command: [ /bin/sh, -c, 'k8pshd --executable-directory=/k8psh && g++ ...' ] # Runs g++ on the centos:6 container
    env:
    - name: K8PSH_CONFIG
      value: /workspace/test/config/k8s.conf
    - name: PATH
      value: '/k8psh:/bin:/usr/bin:/k8psh/bin'
    volumeMounts:
    - mountPath: /k8psh/bin
      name: k8psh-bin
      readOnly: true
  volumes:
  - name: k8psh-bin
    emptyDir:
      medium: Memory
```

The ideal way to install `k8psh` would be to use the k8psh container as a data-only container and mount it as a shared volume. However, this is not possible at the present time due to limitations with Kubernetes.

## What does it solve?
One of the goals of the project is to ease docker image maintenance on highly matrixed workloads. For example, a Java/C++/JNI project has 5 different build environments/images: Alpine, Ubuntu - GCC 5.0+, Ubuntu - GCC 4.X, ARM v7, ARM v8. The project requires gradle, gcc, JDK 8, cmake, and ninja to build.

The build tools could be installed on each individual docker image. However, this can result in different versions of each tool on the different images and can result in a lot of maintenance if tools need to be regularly updated for each image. Image scanning may also be needed on any updated images which may require justifications for using older tools or dependencies that contain vulnerabilities. Additionally, some docker images may come from an upstream source and cannot easily be modified (or may need to be modified every time a new version is received).

Using k8psh, all the existing images can be used as-is, without any changes or updates. An additional image (for example `ubuntu:latest`) is added containing all the common build tools. (In this case gradle, JDK 8, cmake, and ninja.) The k8psh server is configured to run on each container exposing the GCC family of executables. These commands can then be run from the build tools image simply by running `alpine-gcc`, `arm-linux-gnueabihf-gcc`, or even just `gcc`.

The builds now all use the same versions of the build tools, and only one image (the build tools image) needs to be maintained when newer versions of build tools are released. The other images can either be frozen or stripped down into bare or distroless images with only executables and dependent libraries. The k8psh server has no dependencies and can even run on scratch images.

## How it works
One or more k8psh servers are started on different containers in a Pod, each exposing different executables for other containers to run. Each server's exposed executables are listed in a configuration file. When `k8pshd` starts, it generates a set of stub executables (see `--executable-directory`) that can be called just like any other executable. If the server determines that it must expose executables, it will listen for requests from other containers.

When one of the stub executables is called on a client, `k8psh` connects to the appropriate server (as found in the configuration file), requests to start the command on the server, and then transfers data (stdin, stdout, stderr, exit code) as the executable runs. All communication is done via a TCP socket. On the client container, running the stub will appear no different than running the actual program on the client itself.

Multiple servers can be configured, each with different exposed executables. Every server listens on a different port that is determined by the configuration file. The configuration file is typically shared by all containers in a pod.
