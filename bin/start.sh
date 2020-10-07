while true;
do
	cp infclassR.log "logs/infclassr-$(date +"%d-%m-%y-%r").log"
	./server_d -f infclassR.cfg &> /dev/null
	sleep 3
done
