package main

import (
	"html"
	"net/url"
	"sort"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf8"
)

type NodeType int

const (
	DocumentNode NodeType = iota
	ElementNode
	TextNode
	CommentNode
)

type Node struct {
	Type     NodeType
	Data     string
	Attrs    map[string]string
	Parent   *Node
	Children []*Node
	Style    map[string]string
	Computed map[string]string
}

type Document struct {
	Root    *Node
	Styles  Stylesheet
	BaseURL string
	Title   string
}

type Specificity struct {
	A int
	B int
	C int
}

type CSSValue struct {
	Value     string
	Important bool
	Spec      Specificity
	Order     int
	Inline    bool
}

type Declaration struct {
	Property  string
	Value     string
	Important bool
}

type AttrSelector struct {
	Name  string
	Op    string
	Value string
}

type PseudoSelector struct {
	Name    string
	Arg     string
	Element bool
}

type SimpleSelector struct {
	Tag    string
	ID     string
	Class  []string
	Attrs  []AttrSelector
	Pseudo []PseudoSelector
}

type SelectorPart struct {
	Combinator string // "", " ", ">"
	Simple     SimpleSelector
}

type Selector struct {
	Parts       []SelectorPart
	Specificity Specificity
	Raw         string
}

type CSSRule struct {
	Selectors    []Selector
	Declarations []Declaration
	Media        string
	Order        int
}

type Stylesheet struct {
	Rules []CSSRule
}

type StyleContext struct {
	ColorScheme string // "light" or "dark"
	MediaType   string // "screen"
}

var voidElements = map[string]struct{}{
	"area": {}, "base": {}, "br": {}, "col": {}, "embed": {}, "hr": {}, "img": {}, "input": {},
	"link": {}, "meta": {}, "param": {}, "source": {}, "track": {}, "wbr": {},
}

var rawTextElements = map[string]struct{}{
	"script": {}, "style": {}, "textarea": {},
}

var blockElements = map[string]struct{}{
	"address": {}, "article": {}, "aside": {}, "blockquote": {}, "body": {}, "div": {}, "dl": {}, "fieldset": {},
	"figcaption": {}, "figure": {}, "footer": {}, "form": {}, "h1": {}, "h2": {}, "h3": {}, "h4": {}, "h5": {},
	"h6": {}, "header": {}, "hr": {}, "li": {}, "main": {}, "nav": {}, "ol": {}, "p": {}, "pre": {}, "section": {},
	"table": {}, "tbody": {}, "tfoot": {}, "thead": {}, "tr": {}, "td": {}, "th": {}, "ul": {},
	"details": {}, "summary": {}, "menu": {}, "select": {}, "option": {}, "label": {},
}

func newNode(t NodeType, data string) *Node {
	if t == ElementNode || t == DocumentNode {
		data = strings.ToLower(strings.TrimSpace(data))
	}
	return &Node{Type: t, Data: strings.TrimSpace(data)}
}

func (n *Node) appendChild(c *Node) {
	if n == nil || c == nil {
		return
	}
	c.Parent = n
	n.Children = append(n.Children, c)
}

func (n *Node) Attr(name string) string {
	if n == nil || len(n.Attrs) == 0 {
		return ""
	}
	return n.Attrs[strings.ToLower(name)]
}

func (n *Node) TextContent() string {
	if n == nil {
		return ""
	}
	switch n.Type {
	case TextNode:
		return n.Data
	default:
		var b strings.Builder
		for _, c := range n.Children {
			b.WriteString(c.TextContent())
		}
		return b.String()
	}
}

func ParseHTML(src string) *Document {
	doc := &Document{Root: newNode(DocumentNode, "document")}
	if src == "" {
		return doc
	}

	stack := []*Node{doc.Root}
	i := 0
	for i < len(src) {
		if src[i] != '<' {
			j := strings.IndexByte(src[i:], '<')
			if j < 0 {
				j = len(src) - i
			}
			text := src[i : i+j]
			if text != "" {
				stack[len(stack)-1].appendChild(newNode(TextNode, html.UnescapeString(text)))
			}
			i += j
			continue
		}

		if strings.HasPrefix(src[i:], "<!--") {
			if end := strings.Index(src[i+4:], "-->"); end >= 0 {
				i += 4 + end + 3
			} else {
				break
			}
			continue
		}
		if strings.HasPrefix(strings.ToLower(src[i:]), "<!doctype") {
			if end := strings.IndexByte(src[i:], '>'); end >= 0 {
				i += end + 1
				continue
			}
			break
		}
		if strings.HasPrefix(src[i:], "</") {
			name, end := parseClosingTag(src, i)
			if end < 0 {
				i++
				continue
			}
			name = strings.ToLower(name)
			for len(stack) > 1 {
				top := stack[len(stack)-1]
				stack = stack[:len(stack)-1]
				if top.Type == ElementNode && top.Data == name {
					break
				}
			}
			i = end + 1
			continue
		}
		if src[i] == '<' {
			tag, attrs, selfClosing, end := parseOpeningTag(src, i)
			if end < 0 || tag == "" {
				stack[len(stack)-1].appendChild(newNode(TextNode, "<"))
				i++
				continue
			}
			tag = strings.ToLower(tag)
			el := newNode(ElementNode, tag)
			el.Attrs = attrs
			stack[len(stack)-1].appendChild(el)

			if _, ok := rawTextElements[tag]; ok {
				closeTag := "</" + tag + ">"
				lowerRest := strings.ToLower(src[end+1:])
				if idx := strings.Index(lowerRest, strings.ToLower(closeTag)); idx >= 0 {
					raw := src[end+1 : end+1+idx]
					if raw != "" {
						el.appendChild(newNode(TextNode, raw))
					}
					i = end + 1 + idx + len(closeTag)
				} else {
					raw := src[end+1:]
					if raw != "" {
						el.appendChild(newNode(TextNode, raw))
					}
					i = len(src)
				}
				continue
			}

			if !selfClosing {
				if _, ok := voidElements[tag]; !ok {
					stack = append(stack, el)
				}
			}
			i = end + 1
			continue
		}

		i++
	}

	return doc
}

func parseClosingTag(src string, i int) (string, int) {
	end := scanTagEnd(src, i)
	if end < 0 {
		return "", -1
	}
	inner := strings.TrimSpace(src[i+2 : end])
	if inner == "" {
		return "", end
	}
	fields := strings.Fields(inner)
	if len(fields) == 0 {
		return "", end
	}
	return strings.TrimPrefix(strings.ToLower(fields[0]), "/"), end
}

func parseOpeningTag(src string, i int) (string, map[string]string, bool, int) {
	end := scanTagEnd(src, i)
	if end < 0 {
		return "", nil, false, -1
	}
	inner := strings.TrimSpace(src[i+1 : end])
	if inner == "" || inner[0] == '!' || inner[0] == '?' {
		return "", nil, false, end
	}
	selfClosing := false
	if strings.HasSuffix(inner, "/") {
		selfClosing = true
		inner = strings.TrimSpace(strings.TrimSuffix(inner, "/"))
	}
	tag, rest := splitTagName(inner)
	if tag == "" {
		return "", nil, false, end
	}
	attrs := parseAttrs(rest)
	return tag, attrs, selfClosing, end
}

func scanTagEnd(src string, start int) int {
	inSingle, inDouble := false, false
	for i := start + 1; i < len(src); i++ {
		switch src[i] {
		case '\'':
			if !inDouble {
				inSingle = !inSingle
			}
		case '"':
			if !inSingle {
				inDouble = !inDouble
			}
		case '>':
			if !inSingle && !inDouble {
				return i
			}
		}
	}
	return -1
}

func splitTagName(s string) (string, string) {
	s = strings.TrimSpace(s)
	if s == "" {
		return "", ""
	}
	i := 0
	for i < len(s) {
		r, size := utf8.DecodeRuneInString(s[i:])
		if unicode.IsSpace(r) {
			break
		}
		i += size
	}
	return strings.ToLower(s[:i]), strings.TrimSpace(s[i:])
}

func parseAttrs(s string) map[string]string {
	if strings.TrimSpace(s) == "" {
		return map[string]string{}
	}
	attrs := map[string]string{}
	i := 0
	for i < len(s) {
		for i < len(s) {
			r, size := utf8.DecodeRuneInString(s[i:])
			if !unicode.IsSpace(r) {
				break
			}
			i += size
		}
		if i >= len(s) {
			break
		}
		if s[i] == '/' {
			i++
			continue
		}
		start := i
		for i < len(s) {
			r, size := utf8.DecodeRuneInString(s[i:])
			if unicode.IsSpace(r) || r == '=' || r == '/' {
				break
			}
			i += size
		}
		name := strings.ToLower(strings.TrimSpace(s[start:i]))
		if name == "" {
			i++
			continue
		}
		for i < len(s) {
			r, size := utf8.DecodeRuneInString(s[i:])
			if !unicode.IsSpace(r) {
				break
			}
			i += size
		}
		val := name
		if i < len(s) && s[i] == '=' {
			i++
			for i < len(s) {
				r, size := utf8.DecodeRuneInString(s[i:])
				if !unicode.IsSpace(r) {
					break
				}
				i += size
			}
			if i < len(s) {
				switch s[i] {
				case '"', '\'':
					quote := s[i]
					i++
					start = i
					for i < len(s) && s[i] != quote {
						i++
					}
					val = html.UnescapeString(s[start:i])
					if i < len(s) && s[i] == quote {
						i++
					}
				default:
					start = i
					for i < len(s) {
						r, size := utf8.DecodeRuneInString(s[i:])
						if unicode.IsSpace(r) || r == '/' {
							break
						}
						i += size
					}
					val = html.UnescapeString(s[start:i])
				}
			} else {
				val = ""
			}
		}
		attrs[name] = val
	}
	return attrs
}

func ParseCSSStylesheet(src string) Stylesheet {
	src = stripCSSComments(src)
	sheet := Stylesheet{}
	order := 0
	sheet.Rules = append(sheet.Rules, parseCSSRules(src, "", &order)...)
	return sheet
}

func stripCSSComments(src string) string {
	var b strings.Builder
	for i := 0; i < len(src); {
		if i+1 < len(src) && src[i] == '/' && src[i+1] == '*' {
			j := strings.Index(src[i+2:], "*/")
			if j < 0 {
				break
			}
			i += 2 + j + 2
			continue
		}
		b.WriteByte(src[i])
		i++
	}
	return b.String()
}

func parseCSSRules(src, media string, order *int) []CSSRule {
	var rules []CSSRule
	i := 0
	for i < len(src) {
		skipCSSSpace := func() {
			for i < len(src) {
				r, size := utf8.DecodeRuneInString(src[i:])
				if !unicode.IsSpace(r) {
					break
				}
				i += size
			}
		}
		skipCSSSpace()
		if i >= len(src) {
			break
		}
		if src[i] == '@' {
			if strings.HasPrefix(strings.ToLower(src[i:]), "@media") {
				j := i + len("@media")
				for j < len(src) {
					r, size := utf8.DecodeRuneInString(src[j:])
					if r == '{' {
						break
					}
					j += size
				}
				cond := strings.TrimSpace(src[i+len("@media") : j])
				if j >= len(src) || src[j] != '{' {
					break
				}
				body, end := readBalancedBlock(src, j)
				if end < 0 {
					break
				}
				rules = append(rules, parseCSSRules(body, cond, order)...)
				i = end + 1
				continue
			}
			// Skip unknown at-rules
			if j := strings.IndexByte(src[i:], ';'); j >= 0 {
				i += j + 1
				continue
			}
			if j := strings.IndexByte(src[i:], '{'); j >= 0 {
				body, end := readBalancedBlock(src, i+j)
				if end < 0 {
					break
				}
				_ = body
				i = end + 1
				continue
			}
			break
		}

		selEnd := findNextBrace(src, i)
		if selEnd < 0 {
			break
		}
		selectorText := strings.TrimSpace(src[i:selEnd])
		body, end := readBalancedBlock(src, selEnd)
		if end < 0 {
			break
		}
		decls := parseCSSDeclarations(body)
		if selectorText != "" && len(decls) > 0 {
			sels := parseSelectorList(selectorText)
			if len(sels) > 0 {
				rules = append(rules, CSSRule{Selectors: sels, Declarations: decls, Media: strings.TrimSpace(media), Order: *order})
				(*order)++
			}
		}
		i = end + 1
	}
	return rules
}

func findNextBrace(src string, start int) int {
	inSingle, inDouble, depth := false, false, 0
	for i := start; i < len(src); i++ {
		switch src[i] {
		case '\'':
			if !inDouble {
				inSingle = !inSingle
			}
		case '"':
			if !inSingle {
				inDouble = !inDouble
			}
		case '[':
			if !inSingle && !inDouble {
				depth++
			}
		case ']':
			if !inSingle && !inDouble && depth > 0 {
				depth--
			}
		case '{':
			if !inSingle && !inDouble && depth == 0 {
				return i
			}
		case '}':
			if !inSingle && !inDouble && depth == 0 {
				return -1
			}
		}
	}
	return -1
}

func readBalancedBlock(src string, openBrace int) (string, int) {
	if openBrace < 0 || openBrace >= len(src) || src[openBrace] != '{' {
		return "", -1
	}
	inSingle, inDouble, depth := false, false, 0
	for i := openBrace; i < len(src); i++ {
		switch src[i] {
		case '\'':
			if !inDouble {
				inSingle = !inSingle
			}
		case '"':
			if !inSingle {
				inDouble = !inDouble
			}
		case '{':
			if !inSingle && !inDouble {
				depth++
			}
		case '}':
			if !inSingle && !inDouble {
				depth--
				if depth == 0 {
					return src[openBrace+1 : i], i
				}
			}
		}
	}
	return "", -1
}

func parseCSSDeclarations(src string) []Declaration {
	var decls []Declaration
	for _, part := range splitTopLevel(src, ';') {
		if strings.TrimSpace(part) == "" {
			continue
		}
		kv := strings.SplitN(part, ":", 2)
		if len(kv) != 2 {
			continue
		}
		prop := strings.ToLower(strings.TrimSpace(kv[0]))
		val := strings.TrimSpace(kv[1])
		important := false
		lower := strings.ToLower(val)
		if strings.HasSuffix(lower, "!important") {
			important = true
			val = strings.TrimSpace(val[:len(val)-len("!important")])
		}
		if prop != "" && val != "" {
			decls = append(decls, Declaration{Property: prop, Value: val, Important: important})
		}
	}
	return decls
}

func splitTopLevel(src string, sep byte) []string {
	var out []string
	inSingle, inDouble, bracket, paren := false, false, 0, 0
	start := 0
	for i := 0; i < len(src); i++ {
		switch src[i] {
		case '\'':
			if !inDouble {
				inSingle = !inSingle
			}
		case '"':
			if !inSingle {
				inDouble = !inDouble
			}
		case '[':
			if !inSingle && !inDouble {
				bracket++
			}
		case ']':
			if !inSingle && !inDouble && bracket > 0 {
				bracket--
			}
		case '(':
			if !inSingle && !inDouble {
				paren++
			}
		case ')':
			if !inSingle && !inDouble && paren > 0 {
				paren--
			}
		default:
			if src[i] == sep && !inSingle && !inDouble && bracket == 0 && paren == 0 {
				out = append(out, src[start:i])
				start = i + 1
			}
		}
	}
	out = append(out, src[start:])
	return out
}

func parseSelectorList(src string) []Selector {
	parts := splitTopLevel(src, ',')
	out := make([]Selector, 0, len(parts))
	for _, p := range parts {
		sel, ok := parseSelector(strings.TrimSpace(p))
		if ok {
			out = append(out, sel)
		}
	}
	return out
}

func parseSelector(src string) (Selector, bool) {
	if strings.TrimSpace(src) == "" {
		return Selector{}, false
	}
	sel := Selector{Raw: strings.TrimSpace(src)}
	i := 0
	combinator := ""
	for i < len(src) {
		for i < len(src) {
			r, size := utf8.DecodeRuneInString(src[i:])
			if !unicode.IsSpace(r) {
				break
			}
			i += size
		}
		if i >= len(src) {
			break
		}
		if src[i] == '>' {
			combinator = ">"
			i++
			continue
		}
		simple, n := parseSimpleSelector(src[i:])
		if n <= 0 {
			return Selector{}, false
		}
		sel.Parts = append(sel.Parts, SelectorPart{Combinator: combinator, Simple: simple})
		sel.Specificity = addSpec(sel.Specificity, specOf(simple))
		i += n
		combinator = " "
	}
	if len(sel.Parts) == 0 {
		return Selector{}, false
	}
	return sel, true
}

func parseSimpleSelector(src string) (SimpleSelector, int) {
	s := SimpleSelector{}
	i := 0
	if i < len(src) {
		r, size := utf8.DecodeRuneInString(src[i:])
		if r == '*' {
			i += size
		} else if isIdentStart(r) {
			start := i
			i += size
			for i < len(src) {
				r, size = utf8.DecodeRuneInString(src[i:])
				if !isIdentContinue(r) {
					break
				}
				i += size
			}
			s.Tag = strings.ToLower(src[start:i])
		}
	}
	for i < len(src) {
		r, size := utf8.DecodeRuneInString(src[i:])
		switch r {
		case '.':
			i += size
			start := i
			for i < len(src) {
				r, size = utf8.DecodeRuneInString(src[i:])
				if !isIdentContinue(r) {
					break
				}
				i += size
			}
			if start < i {
				s.Class = append(s.Class, strings.ToLower(src[start:i]))
			}
		case '#':
			i += size
			start := i
			for i < len(src) {
				r, size = utf8.DecodeRuneInString(src[i:])
				if !isIdentContinue(r) {
					break
				}
				i += size
			}
			if start < i {
				s.ID = strings.ToLower(src[start:i])
			}
		case '[':
			end := findMatching(src, i, '[', ']')
			if end < 0 {
				return SimpleSelector{}, -1
			}
			content := strings.TrimSpace(src[i+1 : end])
			if content != "" {
				s.Attrs = append(s.Attrs, parseAttrSelector(content))
			}
			i = end + 1
		case ':':
			pseudo, n := parsePseudoSelector(src[i:])
			if n <= 0 {
				return SimpleSelector{}, -1
			}
			s.Pseudo = append(s.Pseudo, pseudo)
			i += n
		default:
			if unicode.IsSpace(r) || r == '>' || r == ',' || r == '{' {
				return s, i
			}
			// stop at other combinator-like characters
			return s, i
		}
	}
	return s, i
}

func parseAttrSelector(src string) AttrSelector {
	a := AttrSelector{}
	src = strings.TrimSpace(src)
	if src == "" {
		return a
	}
	for _, op := range []string{"~=", "|=", "^=", "$=", "*=", "="} {
		if idx := strings.Index(src, op); idx >= 0 {
			a.Name = strings.ToLower(strings.TrimSpace(src[:idx]))
			a.Op = op
			a.Value = unquoteCSS(strings.TrimSpace(src[idx+len(op):]))
			return a
		}
	}
	a.Name = strings.ToLower(strings.TrimSpace(src))
	return a
}

func parsePseudoSelector(src string) (PseudoSelector, int) {
	pseudo := PseudoSelector{}
	i := 0
	if !strings.HasPrefix(src, ":") {
		return pseudo, -1
	}
	for i < len(src) && src[i] == ':' {
		i++
	}
	element := false
	if i < len(src) && src[i] == ':' {
		element = true
		i++
	}
	start := i
	for i < len(src) {
		r, size := utf8.DecodeRuneInString(src[i:])
		if !isIdentContinue(r) {
			break
		}
		i += size
	}
	if start == i {
		return pseudo, -1
	}
	pseudo.Name = strings.ToLower(src[start:i])
	pseudo.Element = element
	if i < len(src) && src[i] == '(' {
		end := findMatching(src, i, '(', ')')
		if end < 0 {
			return pseudo, -1
		}
		pseudo.Arg = strings.TrimSpace(src[i+1 : end])
		i = end + 1
	}
	return pseudo, i
}

func findMatching(src string, start int, open, close byte) int {
	depth := 0
	inSingle, inDouble := false, false
	for i := start; i < len(src); i++ {
		switch src[i] {
		case '\'':
			if !inDouble {
				inSingle = !inSingle
			}
		case '"':
			if !inSingle {
				inDouble = !inDouble
			}
		case open:
			if !inSingle && !inDouble {
				depth++
			}
		case close:
			if !inSingle && !inDouble {
				depth--
				if depth == 0 {
					return i
				}
			}
		}
	}
	return -1
}

func unquoteCSS(s string) string {
	s = strings.TrimSpace(s)
	if len(s) >= 2 {
		if (s[0] == '"' && s[len(s)-1] == '"') || (s[0] == '\'' && s[len(s)-1] == '\'') {
			if unq, err := strconv.Unquote(s); err == nil {
				return unq
			}
			return s[1 : len(s)-1]
		}
	}
	return html.UnescapeString(s)
}

func isIdentStart(r rune) bool {
	return r == '_' || r == '-' || unicode.IsLetter(r)
}

func isIdentContinue(r rune) bool {
	return r == '_' || r == '-' || unicode.IsLetter(r) || unicode.IsDigit(r)
}

func specOf(s SimpleSelector) Specificity {
	sp := Specificity{}
	if s.ID != "" {
		sp.A++
	}
	sp.B += len(s.Class) + len(s.Attrs)
	for _, p := range s.Pseudo {
		if p.Element {
			sp.C++
		} else {
			sp.B++
		}
	}
	if s.Tag != "" && s.Tag != "*" {
		sp.C++
	}
	return sp
}

func addSpec(a, b Specificity) Specificity {
	return Specificity{A: a.A + b.A, B: a.B + b.B, C: a.C + b.C}
}

func compareSpec(a, b Specificity) int {
	switch {
	case a.A != b.A:
		if a.A > b.A {
			return 1
		}
		return -1
	case a.B != b.B:
		if a.B > b.B {
			return 1
		}
		return -1
	case a.C != b.C:
		if a.C > b.C {
			return 1
		}
		return -1
	default:
		return 0
	}
}

func mediaMatches(cond string, ctx StyleContext) bool {
	cond = strings.ToLower(strings.TrimSpace(cond))
	if cond == "" || cond == "all" {
		return true
	}
	if ctx.MediaType == "" {
		ctx.MediaType = "screen"
	}
	if strings.Contains(cond, "print") {
		return ctx.MediaType == "print"
	}
	if strings.Contains(cond, "screen") {
		return ctx.MediaType == "screen" || ctx.MediaType == ""
	}
	if strings.Contains(cond, "prefers-color-scheme") {
		wantDark := strings.Contains(cond, "dark")
		wantLight := strings.Contains(cond, "light")
		if wantDark {
			return strings.ToLower(ctx.ColorScheme) == "dark"
		}
		if wantLight {
			return strings.ToLower(ctx.ColorScheme) != "dark"
		}
	}
	return true
}

func ParseInlineStyle(s string) []Declaration {
	return parseCSSDeclarations(s)
}

func ApplyStyles(doc *Document, sheet Stylesheet, ctx StyleContext) {
	if doc == nil || doc.Root == nil {
		return
	}
	walkNodes(doc.Root, func(n *Node) {
		if n.Type != ElementNode {
			return
		}
		computed := map[string]CSSValue{}
		order := 0
		for _, rule := range sheet.Rules {
			if !mediaMatches(rule.Media, ctx) {
				continue
			}
			matched := false
			var bestSpec Specificity
			for _, sel := range rule.Selectors {
				if matchesSelector(n, sel) {
					matched = true
					if compareSpec(sel.Specificity, bestSpec) > 0 {
						bestSpec = sel.Specificity
					}
				}
			}
			if !matched {
				continue
			}
			for _, decl := range rule.Declarations {
				applyDeclaration(computed, decl.Property, decl.Value, decl.Important, bestSpec, rule.Order)
			}
			order++
		}
		if inline := n.Attr("style"); inline != "" {
			for _, decl := range ParseInlineStyle(inline) {
				applyDeclaration(computed, decl.Property, decl.Value, decl.Important, Specificity{A: 1 << 20}, 1<<30)
			}
		}
		if len(computed) > 0 {
			n.Computed = map[string]string{}
			for k, v := range computed {
				n.Computed[k] = v.Value
			}
			n.Style = n.Computed
		}
	})
}

func applyDeclaration(dst map[string]CSSValue, prop, val string, important bool, spec Specificity, order int) {
	if prop == "" {
		return
	}
	cur, ok := dst[prop]
	cand := CSSValue{Value: val, Important: important, Spec: spec, Order: order}
	if !ok {
		dst[prop] = cand
		return
	}
	if cur.Important != cand.Important {
		if cand.Important {
			dst[prop] = cand
		}
		return
	}
	cmp := compareSpec(cand.Spec, cur.Spec)
	if cmp > 0 || (cmp == 0 && cand.Order >= cur.Order) {
		dst[prop] = cand
	}
}

func matchesSelector(n *Node, sel Selector) bool {
	if n == nil || n.Type != ElementNode || len(sel.Parts) == 0 {
		return false
	}
	return matchSelectorFrom(n, sel.Parts, len(sel.Parts)-1)
}

func matchSelectorFrom(n *Node, parts []SelectorPart, idx int) bool {
	if idx < 0 {
		return true
	}
	if !matchSimple(n, parts[idx].Simple) {
		return false
	}
	if idx == 0 {
		return true
	}
	comb := parts[idx].Combinator
	if comb == ">" {
		return matchSelectorFrom(n.Parent, parts, idx-1)
	}
	for p := n.Parent; p != nil; p = p.Parent {
		if p.Type == ElementNode && matchSelectorFrom(p, parts, idx-1) {
			return true
		}
	}
	return false
}

func matchSimple(n *Node, s SimpleSelector) bool {
	if n == nil || n.Type != ElementNode {
		return false
	}
	if s.Tag != "" && s.Tag != "*" && s.Tag != n.Data {
		return false
	}
	if s.ID != "" && strings.ToLower(n.Attr("id")) != s.ID {
		return false
	}
	classes := map[string]struct{}{}
	for _, c := range strings.Fields(strings.ToLower(n.Attr("class"))) {
		classes[c] = struct{}{}
	}
	for _, cls := range s.Class {
		if _, ok := classes[cls]; !ok {
			return false
		}
	}
	for _, a := range s.Attrs {
		v, ok := n.Attrs[strings.ToLower(a.Name)]
		if !ok {
			return false
		}
		switch a.Op {
		case "":
		case "=":
			if v != a.Value {
				return false
			}
		case "~=":
			found := false
			for _, word := range strings.Fields(v) {
				if word == a.Value {
					found = true
					break
				}
			}
			if !found {
				return false
			}
		case "|=":
			if v != a.Value && !strings.HasPrefix(v, a.Value+"-") {
				return false
			}
		case "^=":
			if !strings.HasPrefix(v, a.Value) {
				return false
			}
		case "$=":
			if !strings.HasSuffix(v, a.Value) {
				return false
			}
		case "*=":
			if !strings.Contains(v, a.Value) {
				return false
			}
		default:
			return false
		}
	}
	for _, p := range s.Pseudo {
		if !matchPseudo(n, p) {
			return false
		}
	}
	return true
}

func matchPseudo(n *Node, p PseudoSelector) bool {
	switch p.Name {
	case "root":
		return n.Parent != nil && n.Parent.Type == DocumentNode
	case "empty":
		for _, c := range n.Children {
			if c.Type == ElementNode || strings.TrimSpace(c.Data) != "" {
				return false
			}
		}
		return true
	case "first-child":
		if n.Parent == nil {
			return false
		}
		for _, c := range n.Parent.Children {
			if c.Type == ElementNode {
				return c == n
			}
		}
		return false
	case "last-child":
		if n.Parent == nil {
			return false
		}
		for i := len(n.Parent.Children) - 1; i >= 0; i-- {
			c := n.Parent.Children[i]
			if c.Type == ElementNode {
				return c == n
			}
		}
		return false
	case "checked":
		return n.Attr("checked") != ""
	case "disabled":
		return n.Attr("disabled") != ""
	case "enabled":
		return n.Attr("disabled") == ""
	case "not":
		if p.Arg == "" {
			return true
		}
		if sel, ok := parseSelector(strings.TrimSpace(p.Arg)); ok {
			return !matchesSelector(n, sel)
		}
		return true
	default:
		return true
	}
}

func walkNodes(n *Node, fn func(*Node)) {
	if n == nil {
		return
	}
	fn(n)
	for _, c := range n.Children {
		walkNodes(c, fn)
	}
}

func IsHidden(n *Node) bool {
	if n == nil || n.Type != ElementNode {
		return false
	}
	if _, ok := n.Attrs["hidden"]; ok {
		return true
	}
	if v := lowerTrim(n.ComputedValue("display")); v == "none" {
		return true
	}
	if v := lowerTrim(n.ComputedValue("visibility")); v == "hidden" {
		return true
	}
	if v := lowerTrim(n.ComputedValue("opacity")); v == "0" {
		return true
	}
	return false
}

func lowerTrim(s string) string {
	return strings.ToLower(strings.TrimSpace(s))
}

func (n *Node) ComputedValue(prop string) string {
	if n == nil {
		return ""
	}
	if n.Computed != nil {
		return n.Computed[strings.ToLower(prop)]
	}
	return ""
}

func QuerySelector(root *Node, selector string) *Node {
	all := QuerySelectorAll(root, selector)
	if len(all) > 0 {
		return all[0]
	}
	return nil
}

func QuerySelectorAll(root *Node, selector string) []*Node {
	if root == nil {
		return nil
	}
	selectors := parseSelectorList(selector)
	if len(selectors) == 0 {
		return nil
	}
	var out []*Node
	seen := map[*Node]struct{}{}
	walkNodes(root, func(n *Node) {
		if n.Type != ElementNode {
			return
		}
		for _, sel := range selectors {
			if matchesSelector(n, sel) {
				if _, ok := seen[n]; !ok {
					seen[n] = struct{}{}
					out = append(out, n)
				}
				break
			}
		}
	})
	return out
}

func CollectText(root *Node) string {
	if root == nil {
		return ""
	}
	var b strings.Builder
	var walk func(*Node)
	walk = func(n *Node) {
		if n == nil {
			return
		}
		if n.Type == TextNode {
			txt := html.UnescapeString(n.Data)
			b.WriteString(txt)
			return
		}
		if n.Type == ElementNode {
			if _, ok := blockElements[n.Data]; ok {
				if b.Len() > 0 {
					last := b.String()
					if !strings.HasSuffix(last, "\n") {
						b.WriteByte('\n')
					}
				}
			}
			for _, c := range n.Children {
				walk(c)
			}
			if _, ok := blockElements[n.Data]; ok {
				if !strings.HasSuffix(b.String(), "\n") {
					b.WriteByte('\n')
				}
			}
			return
		}
		for _, c := range n.Children {
			walk(c)
		}
	}
	walk(root)
	return normalizeTextOutput(b.String())
}

func normalizeTextOutput(s string) string {
	s = strings.ReplaceAll(s, "\r", "")
	s = strings.ReplaceAll(s, "\f", "\n")
	lines := strings.Split(s, "\n")
	out := make([]string, 0, len(lines))
	for _, ln := range lines {
		ln = strings.TrimSpace(ln)
		if ln != "" {
			out = append(out, ln)
		}
	}
	return strings.Join(out, "\n")
}

func ExtractBaseURL(root *Node, fallback *url.URL) *url.URL {
	if root == nil {
		return fallback
	}
	if n := QuerySelector(root, "base[href]"); n != nil {
		if href := strings.TrimSpace(n.Attr("href")); href != "" {
			if u, err := url.Parse(href); err == nil {
				if fallback != nil {
					return fallback.ResolveReference(u)
				}
				return u
			}
		}
	}
	return fallback
}

func ExtractTitle(root *Node) string {
	if root == nil {
		return ""
	}
	if n := QuerySelector(root, "title"); n != nil {
		return strings.TrimSpace(n.TextContent())
	}
	return ""
}

func ExtractLinks(root *Node, base *url.URL) []string {
	if root == nil {
		return nil
	}
	seen := map[string]struct{}{}
	var links []string
	walkNodes(root, func(n *Node) {
		if n.Type != ElementNode || n.Data != "a" || IsHidden(n) {
			return
		}
		href := n.Attr("href")
		if href == "" {
			if js := n.Attr("onclick"); js != "" {
				href = js
			}
		}
		href = strings.TrimSpace(href)
		if href == "" {
			return
		}
		if strings.HasPrefix(strings.ToLower(href), "javascript:") {
			href = extractJSRedirect(href)
		}
		if href == "" {
			return
		}
		if base != nil {
			if u, err := url.Parse(href); err == nil {
				href = base.ResolveReference(u).String()
			}
		}
		if _, ok := seen[href]; ok {
			return
		}
		seen[href] = struct{}{}
		links = append(links, href)
	})
	return links
}

func ExtractForms(root *Node, base *url.URL) []Form {
	var forms []Form
	walkNodes(root, func(n *Node) {
		if n.Type != ElementNode || n.Data != "form" || IsHidden(n) {
			return
		}
		f := Form{Method: strings.ToUpper(strings.TrimSpace(n.Attr("method"))), Action: strings.TrimSpace(n.Attr("action"))}
		if f.Method == "" {
			f.Method = "GET"
		}
		if f.Action == "" && base != nil {
			f.Action = base.String()
		}
		if f.Action != "" && base != nil {
			if u, err := url.Parse(f.Action); err == nil {
				f.Action = base.ResolveReference(u).String()
			}
		}
		walkNodes(n, func(x *Node) {
			if x == n || x.Type != ElementNode || IsHidden(x) {
				return
			}
			switch x.Data {
			case "input":
				typ := strings.ToLower(strings.TrimSpace(x.Attr("type")))
				if typ == "" {
					typ = "text"
				}
				name := x.Attr("name")
				if name == "" {
					return
				}
				val := x.Attr("value")
				if val == "" {
					val = x.Attr("placeholder")
				}
				if (typ == "checkbox" || typ == "radio") && x.Attr("checked") != "" && val == "" {
					val = "on"
				}
				if typ == "submit" || typ == "button" || typ == "image" {
					if val == "" {
						val = typ
					}
					if fa := x.Attr("formaction"); fa != "" {
						if base != nil {
							if u, err := url.Parse(fa); err == nil {
								f.Action = base.ResolveReference(u).String()
							}
						} else {
							f.Action = fa
						}
					}
					if fm := strings.ToUpper(strings.TrimSpace(x.Attr("formmethod"))); fm != "" {
						f.Method = fm
					}
				}
				f.Fields = append(f.Fields, Field{Name: name, Type: typ, Value: val, Enabled: x.Attr("disabled") == ""})
			case "textarea":
				name := x.Attr("name")
				if name == "" {
					return
				}
				f.Fields = append(f.Fields, Field{Name: name, Type: "textarea", Value: strings.TrimSpace(x.TextContent()), Enabled: x.Attr("disabled") == ""})
			case "select":
				name := x.Attr("name")
				if name == "" {
					return
				}
				val := selectedOptionValue(x)
				f.Fields = append(f.Fields, Field{Name: name, Type: "select", Value: val, Enabled: x.Attr("disabled") == ""})
			case "button":
				typ := strings.ToLower(strings.TrimSpace(x.Attr("type")))
				if typ == "" {
					typ = "button"
				}
				name := x.Attr("name")
				if name == "" {
					return
				}
				val := x.Attr("value")
				if val == "" {
					val = strings.TrimSpace(x.TextContent())
				}
				if val == "" {
					val = typ
				}
				if fa := x.Attr("formaction"); fa != "" {
					if base != nil {
						if u, err := url.Parse(fa); err == nil {
							f.Action = base.ResolveReference(u).String()
						}
					} else {
						f.Action = fa
					}
				}
				if fm := strings.ToUpper(strings.TrimSpace(x.Attr("formmethod"))); fm != "" {
					f.Method = fm
				}
				f.Fields = append(f.Fields, Field{Name: name, Type: typ, Value: val, Enabled: x.Attr("disabled") == ""})
			}
		})
		forms = append(forms, f)
	})
	return forms
}

func selectedOptionValue(sel *Node) string {
	if sel == nil {
		return ""
	}
	var fallback string
	for _, c := range sel.Children {
		if c.Type != ElementNode || c.Data != "option" {
			continue
		}
		val := c.Attr("value")
		if val == "" {
			val = strings.TrimSpace(c.TextContent())
		}
		if fallback == "" {
			fallback = val
		}
		if c.Attr("selected") != "" {
			return val
		}
	}
	return fallback
}

func DOMAndCSSFromHTML(src string, base *url.URL, ctx StyleContext) (*Document, *url.URL) {
	doc := ParseHTML(src)
	if doc == nil {
		return nil, base
	}
	var css strings.Builder
	walkNodes(doc.Root, func(n *Node) {
		if n.Type == ElementNode && n.Data == "style" {
			css.WriteString(n.TextContent())
			css.WriteByte('\n')
		}
	})
	doc.Styles = ParseCSSStylesheet(css.String())
	if ctx.MediaType == "" {
		ctx.MediaType = "screen"
	}
	ApplyStyles(doc, doc.Styles, ctx)
	base = ExtractBaseURL(doc.Root, base)
	return doc, base
}

func SortedSelectorStrings(sels []Selector) []string {
	out := make([]string, 0, len(sels))
	for _, s := range sels {
		out = append(out, s.Raw)
	}
	sort.Strings(out)
	return out
}
