# development enviroment
FROM alpine:3.4 AS development
WORKDIR /src
COPY . .
# speed up apk downloading (for China mainland)
RUN sed -i 's/dl-cdn.alpinelinux.org/mirrors.ustc.edu.cn/g' /etc/apk/repositories
RUN apk update && apk upgrade
RUN apk add --no-cache openssl gcc g++ make cmake python bam icu-dev libmaxminddb-dev
RUN bam server_release

# production enviroment (alpine
FROM alpine:3.4 AS production
WORKDIR /teeworlds_srv/
# speed up apk downloading (for China mainland)
RUN sed -i 's/dl-cdn.alpinelinux.org/mirrors.ustc.edu.cn/g' /etc/apk/repositories
RUN apk update && apk upgrade
RUN apk add --no-cache openssl libstdc++ libmaxminddb
COPY --from=development /src/bin ./infclass_srv
EXPOSE 8303/udp
ENTRYPOINT ["./server"]
