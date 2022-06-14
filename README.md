# TalkRoom
This is an online talkroom.
环境配置：Ubuntu 16.04

① 需要把 server 和 client 分别部署到不同的虚拟机上，server 有两种不同的实现方式，具体区别有待后续补充，可先自行查看代码

② 在各自的终端运行 g++ filename.cpp -o filename（filename为对应的文件名），进行编译

③ 在 server 端运行 ifconfig 查看其 ipv4 地址，假设为 ipServer，后运行 ./filename ipServer 8003

④ 在各个 client 端运行 ./filename ipServer 8003，至此客户端和服务器端已连通，各客户端可进行在线聊天
