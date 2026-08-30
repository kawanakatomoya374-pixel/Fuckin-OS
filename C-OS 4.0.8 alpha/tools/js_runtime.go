package main

import (
	"html"
	"net/url"
	"strings"
)

// ExecuteInlineScripts walks inline <script> elements and applies a very small
// subset of JavaScript-driven DOM mutations and redirects.
func ExecuteInlineScripts(doc *Document, base *url.URL) string {
	if doc == nil || doc.Root == nil {
		return ""
	}
	var redirect string
	walkNodes(doc.Root, func(n *Node) {
		if redirect != "" || n == nil || n.Type != ElementNode || n.Data != "script" {
			return
		}
		if src := strings.TrimSpace(n.Attr("src")); src != "" {
			return
		}
		if r := runInlineScript(doc, n.TextContent(), base); r != "" {
			redirect = r
		}
	})
	return redirect
}

func runInlineScript(doc *Document, code string, base *url.URL) string {
	code = stripJSComments(code)
	for _, stmt := range splitJSStatements(code) {
		if redirect := applyJSStatement(doc, strings.TrimSpace(stmt), base); redirect != "" {
			return redirect
		}
	}
	return ""
}

func stripJSComments(src string) string {
	var b strings.Builder
	inSingle, inDouble, inBacktick := false, false, false
	for i := 0; i < len(src); i++ {
		ch := src[i]
		if ch == '\\' && (inSingle || inDouble || inBacktick) && i+1 < len(src) {
			b.WriteByte(ch)
			i++
			b.WriteByte(src[i])
			continue
		}
		switch ch {
		case '\'':
			if !inDouble && !inBacktick {
				inSingle = !inSingle
			}
		case '"':
			if !inSingle && !inBacktick {
				inDouble = !inDouble
			}
		case '`':
			if !inSingle && !inDouble {
				inBacktick = !inBacktick
			}
		case '/':
			if !inSingle && !inDouble && !inBacktick && i+1 < len(src) {
				next := src[i+1]
				if next == '/' {
					for i+2 < len(src) && src[i+2] != '\n' {
						i++
					}
					continue
				}
				if next == '*' {
					i += 2
					for i+1 < len(src) && !(src[i] == '*' && src[i+1] == '/') {
						i++
					}
					i++
					continue
				}
			}
		}
		b.WriteByte(ch)
	}
	return b.String()
}

func splitJSStatements(src string) []string {
	var out []string
	start := 0
	inSingle, inDouble, inBacktick := false, false, false
	paren, bracket, brace := 0, 0, 0
	for i := 0; i < len(src); i++ {
		ch := src[i]
		if ch == '\\' && (inSingle || inDouble || inBacktick) && i+1 < len(src) {
			i++
			continue
		}
		switch ch {
		case '\'':
			if !inDouble && !inBacktick {
				inSingle = !inSingle
			}
		case '"':
			if !inSingle && !inBacktick {
				inDouble = !inDouble
			}
		case '`':
			if !inSingle && !inDouble {
				inBacktick = !inBacktick
			}
		case '(':
			if !inSingle && !inDouble && !inBacktick {
				paren++
			}
		case ')':
			if !inSingle && !inDouble && !inBacktick && paren > 0 {
				paren--
			}
		case '[':
			if !inSingle && !inDouble && !inBacktick {
				bracket++
			}
		case ']':
			if !inSingle && !inDouble && !inBacktick && bracket > 0 {
				bracket--
			}
		case '{':
			if !inSingle && !inDouble && !inBacktick {
				brace++
			}
		case '}':
			if !inSingle && !inDouble && !inBacktick && brace > 0 {
				brace--
			}
		case ';':
			if !inSingle && !inDouble && !inBacktick && paren == 0 && bracket == 0 && brace == 0 {
				out = append(out, src[start:i])
				start = i + 1
			}
		}
	}
	if start < len(src) {
		out = append(out, src[start:])
	}
	return out
}

func applyJSStatement(doc *Document, stmt string, base *url.URL) string {
	stmt = strings.TrimSpace(stmt)
	if stmt == "" {
		return ""
	}
	lower := strings.ToLower(stmt)

	if strings.HasPrefix(lower, "document.title") {
		if eq := strings.Index(stmt, "="); eq >= 0 {
			if v, ok := parseJSLiteral(strings.TrimSpace(stmt[eq+1:])); ok {
				setDocumentTitle(doc, v)
			}
		}
		return ""
	}

	if target := extractRedirectFromJS(stmt); target != "" {
		return resolveScriptURL(target, base)
	}

	if strings.HasPrefix(lower, "document.write(") {
		if v, ok := parseCallArgument(stmt, "document.write"); ok {
			appendHTMLToNode(documentBody(doc), v)
		}
		return ""
	}

	if strings.HasPrefix(lower, "document.body.innerhtml") {
		if v, ok := parseAssignmentValue(stmt); ok {
			setNodeHTML(documentBody(doc), v)
		}
		return ""
	}
	if strings.HasPrefix(lower, "document.body.textcontent") {
		if v, ok := parseAssignmentValue(stmt); ok {
			setNodeText(documentBody(doc), v)
		}
		return ""
	}

	if redirect := applyNodeMutation(stmt, doc); redirect != "" {
		return redirect
	}

	return ""
}

func applyNodeMutation(stmt string, doc *Document) string {
	stmt = strings.TrimSpace(stmt)
	if stmt == "" {
		return ""
	}
	lower := strings.ToLower(stmt)
	var target *Node
	var rest string
	if strings.HasPrefix(lower, "document.getelementbyid(") {
		id, after, ok := parseSelectorCall(stmt, "document.getElementById")
		if !ok {
			return ""
		}
		target = findElementByID(doc.Root, id)
		rest = strings.TrimSpace(after)
	} else if strings.HasPrefix(lower, "document.queryselector(") {
		sel, after, ok := parseSelectorCall(stmt, "document.querySelector")
		if !ok {
			return ""
		}
		target = QuerySelector(doc.Root, sel)
		rest = strings.TrimSpace(after)
	} else {
		return ""
	}
	if target == nil || rest == "" || !strings.HasPrefix(rest, ".") {
		return ""
	}
	rest = rest[1:]
	lower = strings.ToLower(rest)
	if strings.HasPrefix(lower, "textcontent") {
		if v, ok := parseAssignmentValue(rest); ok {
			setNodeText(target, v)
		}
		return ""
	}
	if strings.HasPrefix(lower, "innerhtml") {
		if v, ok := parseAssignmentValue(rest); ok {
			setNodeHTML(target, v)
		}
		return ""
	}
	if strings.HasPrefix(lower, "classname") {
		if v, ok := parseAssignmentValue(rest); ok {
			if target.Attrs == nil {
				target.Attrs = map[string]string{}
			}
			target.Attrs["class"] = v
		}
		return ""
	}
	if strings.HasPrefix(lower, "id") {
		if v, ok := parseAssignmentValue(rest); ok {
			if target.Attrs == nil {
				target.Attrs = map[string]string{}
			}
			target.Attrs["id"] = v
		}
		return ""
	}
	if strings.HasPrefix(lower, "value") {
		if v, ok := parseAssignmentValue(rest); ok {
			if target.Attrs == nil {
				target.Attrs = map[string]string{}
			}
			target.Attrs["value"] = v
		}
		return ""
	}
	if strings.HasPrefix(lower, "style.") {
		prop, v, ok := parseStyleAssignment(rest)
		if ok {
			if target.Computed == nil {
				target.Computed = map[string]string{}
			}
			target.Computed[strings.ToLower(prop)] = v
			target.Style = target.Computed
		}
		return ""
	}
	if strings.HasPrefix(lower, "setattribute(") {
		args, _, ok := parseFunctionCall(rest, "setAttribute")
		if ok && len(args) >= 2 {
			if target.Attrs == nil {
				target.Attrs = map[string]string{}
			}
			target.Attrs[strings.ToLower(args[0])] = args[1]
		}
	}
	return ""
}

func parseSelectorCall(stmt, funcName string) (string, string, bool) {
	start := strings.Index(strings.ToLower(stmt), strings.ToLower(funcName)+"(")
	if start < 0 {
		return "", "", false
	}
	args, rest, ok := parseFunctionCall(stmt[start:], funcName)
	if !ok || len(args) == 0 {
		return "", "", false
	}
	v, ok := parseJSLiteral(args[0])
	if !ok {
		v = args[0]
	}
	return v, rest, true
}

func parseFunctionCall(stmt, funcName string) ([]string, string, bool) {
	idx := strings.Index(strings.ToLower(stmt), strings.ToLower(funcName)+"(")
	if idx < 0 {
		return nil, "", false
	}
	open := idx + len(funcName)
	if open >= len(stmt) || stmt[open] != '(' {
		return nil, "", false
	}
	end := findMatchingParen(stmt, open)
	if end < 0 {
		return nil, "", false
	}
	return splitJSArguments(stmt[open+1:end]), strings.TrimSpace(stmt[end+1:]), true
}

func findMatchingParen(src string, open int) int {
	depth := 0
	inSingle, inDouble, inBacktick := false, false, false
	for i := open; i < len(src); i++ {
		ch := src[i]
		if ch == '\\' && (inSingle || inDouble || inBacktick) && i+1 < len(src) {
			i++
			continue
		}
		switch ch {
		case '\'':
			if !inDouble && !inBacktick {
				inSingle = !inSingle
			}
		case '"':
			if !inSingle && !inBacktick {
				inDouble = !inDouble
			}
		case '`':
			if !inSingle && !inDouble {
				inBacktick = !inBacktick
			}
		case '(':
			if !inSingle && !inDouble && !inBacktick {
				depth++
			}
		case ')':
			if !inSingle && !inDouble && !inBacktick {
				depth--
				if depth == 0 {
					return i
				}
			}
		}
	}
	return -1
}

func splitJSArguments(src string) []string {
	var out []string
	start := 0
	inSingle, inDouble, inBacktick := false, false, false
	paren, bracket, brace := 0, 0, 0
	for i := 0; i < len(src); i++ {
		ch := src[i]
		if ch == '\\' && (inSingle || inDouble || inBacktick) && i+1 < len(src) {
			i++
			continue
		}
		switch ch {
		case '\'':
			if !inDouble && !inBacktick {
				inSingle = !inSingle
			}
		case '"':
			if !inSingle && !inBacktick {
				inDouble = !inDouble
			}
		case '`':
			if !inSingle && !inDouble {
				inBacktick = !inBacktick
			}
		case '(':
			if !inSingle && !inDouble && !inBacktick {
				paren++
			}
		case ')':
			if !inSingle && !inDouble && !inBacktick && paren > 0 {
				paren--
			}
		case '[':
			if !inSingle && !inDouble && !inBacktick {
				bracket++
			}
		case ']':
			if !inSingle && !inDouble && !inBacktick && bracket > 0 {
				bracket--
			}
		case '{':
			if !inSingle && !inDouble && !inBacktick {
				brace++
			}
		case '}':
			if !inSingle && !inDouble && !inBacktick && brace > 0 {
				brace--
			}
		case ',':
			if !inSingle && !inDouble && !inBacktick && paren == 0 && bracket == 0 && brace == 0 {
				out = append(out, src[start:i])
				start = i + 1
			}
		}
	}
	if start < len(src) {
		out = append(out, src[start:])
	}
	for i := range out {
		out[i] = strings.TrimSpace(out[i])
	}
	return out
}

func parseJSLiteral(s string) (string, bool) {
	s = strings.TrimSpace(s)
	if s == "" {
		return "", false
	}
	q := s[0]
	if q != '\'' && q != '"' && q != '`' {
		return html.UnescapeString(s), true
	}
	if len(s) < 2 || s[len(s)-1] != q {
		return "", false
	}
	var b strings.Builder
	for i := 1; i < len(s)-1; i++ {
		ch := s[i]
		if ch != '\\' || i+1 >= len(s)-1 {
			b.WriteByte(ch)
			continue
		}
		i++
		switch s[i] {
		case 'n':
			b.WriteByte('\n')
		case 'r':
			b.WriteByte('\r')
		case 't':
			b.WriteByte('\t')
		case '\\':
			b.WriteByte('\\')
		case '\'':
			b.WriteByte('\'')
		case '"':
			b.WriteByte('"')
		case 'b':
			b.WriteByte('\b')
		case 'f':
			b.WriteByte('\f')
		case '0':
			b.WriteByte(0)
		default:
			b.WriteByte(s[i])
		}
	}
	return html.UnescapeString(b.String()), true
}

func parseAssignmentValue(rest string) (string, bool) {
	idx := strings.Index(rest, "=")
	if idx < 0 {
		return "", false
	}
	return parseJSLiteral(strings.TrimSpace(rest[idx+1:]))
}

func parseStyleAssignment(rest string) (prop, value string, ok bool) {
	idx := strings.Index(rest, "=")
	if idx < 0 {
		return "", "", false
	}
	prop = strings.TrimSpace(rest[len("style."):idx])
	value, ok = parseJSLiteral(strings.TrimSpace(rest[idx+1:]))
	return
}

func parseCallArgument(stmt, funcName string) (string, bool) {
	args, _, ok := parseFunctionCall(stmt, funcName)
	if !ok || len(args) == 0 {
		return "", false
	}
	return parseJSLiteral(args[0])
}

func extractRedirectFromJS(stmt string) string {
	lower := strings.ToLower(strings.TrimSpace(stmt))
	if strings.HasPrefix(lower, "location.assign(") {
		if v, ok := parseCallArgument(stmt, "location.assign"); ok {
			return v
		}
	}
	if strings.HasPrefix(lower, "location.replace(") {
		if v, ok := parseCallArgument(stmt, "location.replace"); ok {
			return v
		}
	}
	if idx := strings.Index(lower, "location.href"); idx >= 0 {
		if eq := strings.Index(stmt[idx:], "="); eq >= 0 {
			if v, ok := parseJSLiteral(strings.TrimSpace(stmt[idx+eq+1:])); ok {
				return v
			}
		}
	}
	if idx := strings.Index(lower, "window.location"); idx >= 0 {
		if eq := strings.Index(stmt[idx:], "="); eq >= 0 {
			if v, ok := parseJSLiteral(strings.TrimSpace(stmt[idx+eq+1:])); ok {
				return v
			}
		}
	}
	if idx := strings.Index(lower, "document.location"); idx >= 0 {
		if eq := strings.Index(stmt[idx:], "="); eq >= 0 {
			if v, ok := parseJSLiteral(strings.TrimSpace(stmt[idx+eq+1:])); ok {
				return v
			}
		}
	}
	if idx := strings.Index(lower, "top.location"); idx >= 0 {
		if eq := strings.Index(stmt[idx:], "="); eq >= 0 {
			if v, ok := parseJSLiteral(strings.TrimSpace(stmt[idx+eq+1:])); ok {
				return v
			}
		}
	}
	return ""
}

func resolveScriptURL(raw string, base *url.URL) string {
	raw = strings.TrimSpace(html.UnescapeString(raw))
	if raw == "" {
		return ""
	}
	if base != nil {
		if u, err := url.Parse(raw); err == nil {
			return base.ResolveReference(u).String()
		}
	}
	return raw
}

func documentBody(doc *Document) *Node {
	if doc == nil || doc.Root == nil {
		return nil
	}
	if n := QuerySelector(doc.Root, "body"); n != nil {
		return n
	}
	return doc.Root
}

func setDocumentTitle(doc *Document, title string) {
	if doc == nil {
		return
	}
	doc.Title = strings.TrimSpace(title)
	if n := QuerySelector(doc.Root, "title"); n != nil {
		setNodeText(n, title)
	}
}

func findElementByID(root *Node, id string) *Node {
	if root == nil {
		return nil
	}
	id = strings.TrimSpace(id)
	if id == "" {
		return nil
	}
	var found *Node
	walkNodes(root, func(n *Node) {
		if found != nil || n == nil || n.Type != ElementNode {
			return
		}
		if n.Attr("id") == id {
			found = n
		}
	})
	return found
}

func setNodeText(n *Node, text string) {
	if n == nil {
		return
	}
	n.Children = nil
	n.appendChild(newNode(TextNode, html.UnescapeString(text)))
}

func setNodeHTML(n *Node, src string) {
	if n == nil {
		return
	}
	n.Children = nil
	appendHTMLToNode(n, src)
}

func appendHTMLToNode(n *Node, src string) {
	if n == nil || strings.TrimSpace(src) == "" {
		return
	}
	frag := ParseHTML("<fragment>" + src + "</fragment>")
	if frag == nil || frag.Root == nil {
		n.appendChild(newNode(TextNode, html.UnescapeString(src)))
		return
	}
	var wrapper *Node
	for _, c := range frag.Root.Children {
		if c.Type == ElementNode && c.Data == "fragment" {
			wrapper = c
			break
		}
	}
	if wrapper == nil {
		n.appendChild(newNode(TextNode, html.UnescapeString(src)))
		return
	}
	for _, c := range wrapper.Children {
		c.Parent = n
		n.Children = append(n.Children, c)
	}
}
