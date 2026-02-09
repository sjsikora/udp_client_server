#ifndef UTCP_INPUT_H
#define UTCP_INPUT_H

/**
 * @brief Constantly listen and handle packets
 *
 * The utcp_input function serves as the de facto listen function. It will be allocated
 * with a thread to be constantly running.
 *
 * On packet arrival, we first deserialize it. Then, what happens depends on the state of
 * the TCB. Non-established states all get their own special handling to process SYN and
 * later down the line, FIN bytes.
 *
 * Then we move to handle the data. When a packet comes in, we must update the state of the
 * TCB. A packet will contain a sequence number of which we need to write to our recieve buffer
 * and then update recieve state, and a packet will contain an acknlowgement of the data we went.
 * Of which we need to handle a bit for carefully.
 *
 * Finally, if needed, the input will send a request to TCP_INPUT to ack.
 */
int utcp_input(struct tcb *tcb);

#endif
