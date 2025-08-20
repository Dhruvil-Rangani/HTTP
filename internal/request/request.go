package request

import (
	"errors"
	"fmt"
	"io"
	"strings"
)

type parserState string
const (
	StateInit parserState= "init"
	StateDone parserState = "done"
)

type RequestLine struct {
	HttpVersion   string
	RequestTarget string
	Method        string
}

type Request struct {
	RequestLine RequestLine
	state parserState
}

func newRequest() *Request {
	return &Request {
		state: StateInit,
	}
}

var ERROR_MALFORMED_REQUEST_LINE = fmt.Errorf("malformed request-line")
var ERROR_UNSUPPORTED_HTTP_VERSION = fmt.Errorf("unsupproted http version")
var SEPARATOR = "\r\n"

func parseRequestLine(b string) (*RequestLine, int, error) {
	idx := strings.Index(b, SEPARATOR)
	if idx == -1 {
		return nil, b, nil
	}

	startLine := b[:idx]
	restOfMsg := b[idx + len(SEPARATOR):]

	parts := strings.Split(startLine, " ")
	if len(parts) != 3 {
		return nil, restOfMsg, ERROR_MALFORMED_REQUEST_LINE
	}

	httpParts := strings.Split(parts[2], "/")
	if len(httpParts) != 2 || httpParts[0] != "HTTP" || httpParts[1] != "1.1" {
		return nil, restOfMsg, ERROR_MALFORMED_REQUEST_LINE
	}

	rl := &RequestLine{
		Method: parts[0],
		RequestTarget: parts[1],
		HttpVersion: httpParts[1],
	}
	
	return rl, restOfMsg, nil
}

func RequestFromReader(reader io.Reader) (*Request, error) {
	request := newRequest()

	buf := make([] byte, 1024)
	bufIdx := 0
	for{
		n, err := reader.Read(buf[bufIdx:])
		if err != nil {
			return nil, err
		}
		request.parse(buf[:bufIdx + 1])
	}

	if err != nil {
		return nil, errors.Join(
			fmt.Errorf("unable to read io.ReadAll"),
			err,
		)
	}

	str := string(data)
	rl, _ , err := parseRequestLine(str)
	if err != nil {
		return nil, err
	}
	return &Request{
		RequestLine: *rl,
	}, err
}
