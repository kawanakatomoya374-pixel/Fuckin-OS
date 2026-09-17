package main

import (
	"bufio"
	"flag"
	"fmt"
	"html"
	"io"
	"mime"
	"net/http"
	"net/http/cookiejar"
	neturl "net/url"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

type Field struct {
	Name    string
	Type    string
	Value   string
	Enabled bool
}

type Form struct {
	Method string
	Action string
	Fields []Field
}

type page struct {
	URL         string
	Title       string
	Text        string
	Links       []string
	Forms       []Form
	ContentType string
	StatusCode  int
	Fetched     time.Time
}

type browser struct {
	history []page
	pos     int
	client  *http.Client
}

var (
	scriptStyleRe = regexp.MustCompile(`(?is)<(script|style|noscript)[^>]*>.*?</(script|style|noscript)>`)
	commentRe     = regexp.MustCompile(`(?is)<!--.*?-->`)
	blockRe       = regexp.MustCompile(`(?is)</?(p|div|h[1-6]|li|tr|section|article|br|hr|table|ul|ol|blockquote|pre|header|footer|nav|main|aside|thead|tbody|tfoot|td|th|form|fieldset|legend|figure|figcaption|section|article|header|footer|details|summary)[^>]*>`)
	tagRe         = regexp.MustCompile(`(?is)<[^>]+>`)
	linkRe        = regexp.MustCompile(`(?is)<a\b[^>]*href\s*=\s*["']?([^"' >]+)["']?[^>]*>(.*?)</a>`)
	titleRe       = regexp.MustCompile(`(?is)<title[^>]*>(.*?)</title>`)
	baseHrefRe    = regexp.MustCompile(`(?is)<base\b[^>]*href\s*=\s*["']?([^"' >]+)["']?[^>]*>`)
	anchorTagRe   = regexp.MustCompile(`(?is)<a\b([^>]*)>(.*?)</a>`)
	buttonTagRe   = regexp.MustCompile(`(?is)<button\b([^>]*)>(.*?)</button>`)
	jsURLRe1      = regexp.MustCompile(`(?is)(?:location|window\.location|document\.location|top\.location)(?:\.href)?\s*=\s*["']([^"']+)["']`)
	jsURLRe2      = regexp.MustCompile(`(?is)location\.(?:replace|assign)\s*\(\s*["']([^"']+)["']`)
	jsURLRe3      = regexp.MustCompile(`(?is)window\.open\s*\(\s*["']([^"']+)["']`)
	formOpenRe    = regexp.MustCompile(`(?is)<form\b([^>]*)>`)
	formCloseRe   = regexp.MustCompile(`(?is)</form>`)
	inputRe       = regexp.MustCompile(`(?is)<input\b([^>]*)>`)
	textareaRe    = regexp.MustCompile(`(?is)<textarea\b([^>]*)>(.*?)</textarea>`)
	selectRe      = regexp.MustCompile(`(?is)<select\b([^>]*)>(.*?)</select>`)
	optionRe      = regexp.MustCompile(`(?is)<option\b([^>]*)>(.*?)</option>`)
	buttonRe      = regexp.MustCompile(`(?is)<button\b([^>]*)>(.*?)</button>`)
	checkedRe     = regexp.MustCompile(`(?is)\bchecked\b`)
	selectedRe    = regexp.MustCompile(`(?is)\bselected\b`)
	spaceRe       = regexp.MustCompile(`[ \t]+`)
	blankRe       = regexp.MustCompile(`\n{3,}`)
	attrCache     sync.Map
)

func newBrowser() *browser {
	jar, _ := cookiejar.New(nil)
	transport := &http.Transport{
		Proxy: http.ProxyFromEnvironment,
	}
	return &browser{client: &http.Client{
		Timeout:   20 * time.Second,
		Jar:       jar,
		Transport: transport,
	}}
}

func cleanInline(s string) string {
	s = html.UnescapeString(strings.TrimSpace(s))
	s = strings.ReplaceAll(s, "\t", " ")
	s = spaceRe.ReplaceAllString(s, " ")
	return strings.TrimSpace(s)
}

func resolveAgainst(ref string, baseURL string) (string, error) {
	ref = strings.TrimSpace(ref)
	if ref == "" {
		return "", fmt.Errorf("empty url")
	}
	base, err := neturl.Parse(baseURL)
	if err != nil {
		return "", err
	}
	u, err := neturl.Parse(ref)
	if err != nil {
		return "", err
	}
	return base.ResolveReference(u).String(), nil
}

func normalizeURL(raw string, base *neturl.URL) (string, *neturl.URL, error) {
	u := strings.TrimSpace(raw)
	if u == "" {
		return "", nil, fmt.Errorf("empty url")
	}
	parsed, err := neturl.Parse(u)
	if err != nil {
		return "", nil, err
	}
	if parsed.Scheme == "" {
		if base != nil {
			parsed = base.ResolveReference(parsed)
		} else {
			parsed.Scheme = "https"
		}
	}
	parsed.Fragment = ""
	return parsed.String(), parsed, nil
}

func extractAttr(attrs, key string) string {
	if attrs == "" || key == "" {
		return ""
	}
	if v, ok := attrCache.Load(key); ok {
		if re, ok := v.(*regexp.Regexp); ok {
			if m := re.FindStringSubmatch(attrs); len(m) > 1 {
				for _, g := range m[1:] {
					if g != "" {
						return html.UnescapeString(strings.TrimSpace(g))
					}
				}
			}
			return ""
		}
	}
	pattern := `(?is)\b` + regexp.QuoteMeta(key) + `\b\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'>]+))`
	re := regexp.MustCompile(pattern)
	attrCache.Store(key, re)
	if m := re.FindStringSubmatch(attrs); len(m) > 1 {
		for _, g := range m[1:] {
			if g != "" {
				return html.UnescapeString(strings.TrimSpace(g))
			}
		}
	}
	return ""
}

func hasAttr(attrs, key string) bool {
	if attrs == "" || key == "" {
		return false
	}
	pattern := `(?is)\b` + regexp.QuoteMeta(key) + `\b`
	return regexp.MustCompile(pattern).FindStringIndex(attrs) != nil
}

func stripTagsHTML(src string) string {
	src = commentRe.ReplaceAllString(src, "")
	src = regexp.MustCompile(`(?is)<(head|script|style|noscript)\b[^>]*>.*?</(head|script|style|noscript)>`).ReplaceAllString(src, "")
	src = regexp.MustCompile(`(?is)<li\b[^>]*>`).ReplaceAllString(src, "\n- ")
	src = regexp.MustCompile(`(?is)<(br|hr)\b[^>]*>`).ReplaceAllString(src, "\n")
	src = regexp.MustCompile(`(?is)</?(p|div|h[1-6]|tr|section|article|table|ul|ol|blockquote|pre|header|footer|nav|main|aside|thead|tbody|tfoot|td|th|form|fieldset|legend|figure|figcaption|details|summary)\b[^>]*>`).ReplaceAllString(src, "\n")
	src = tagRe.ReplaceAllString(src, "")
	src = html.UnescapeString(src)
	src = strings.ReplaceAll(src, "\r", "")
	src = strings.ReplaceAll(src, "\f", "\n")
	lines := strings.Split(src, "\n")
	out := make([]string, 0, len(lines))
	for _, ln := range lines {
		ln = cleanInline(ln)
		if ln != "" {
			out = append(out, ln)
		}
	}
	text := strings.Join(out, "\n")
	return blankRe.ReplaceAllString(text, "\n\n")
}

func isHiddenStyle(attrs string) bool {
	style := strings.ToLower(extractAttr(attrs, "style"))
	return strings.Contains(style, "display:none") || strings.Contains(style, "visibility:hidden") || strings.Contains(style, "opacity:0")
}

func extractJSRedirect(js string) string {
	js = html.UnescapeString(strings.TrimSpace(js))
	if js == "" {
		return ""
	}
	lower := strings.ToLower(js)
	if strings.HasPrefix(lower, "javascript:") {
		js = strings.TrimSpace(js[len("javascript:"):])
		lower = strings.ToLower(js)
	}
	if js == "" {
		return ""
	}
	for _, re := range []*regexp.Regexp{jsURLRe1, jsURLRe2, jsURLRe3} {
		if m := re.FindStringSubmatch(js); len(m) > 1 {
			return strings.TrimSpace(html.UnescapeString(m[1]))
		}
	}
	// Tiny fallback for common quoted forms: location.href('url') or return 'url'
	if idx := strings.Index(lower, "location.href"); idx >= 0 {
		if q := strings.IndexAny(js[idx:], "\"'"); q >= 0 {
			js2 := js[idx+q+1:]
			if e := strings.IndexAny(js2, "\"'"); e >= 0 {
				return strings.TrimSpace(js2[:e])
			}
		}
	}
	return ""
}

func resolveMaybeRelative(href string, base *neturl.URL) string {
	href = strings.TrimSpace(html.UnescapeString(href))
	if href == "" {
		return ""
	}
	if strings.HasPrefix(strings.ToLower(href), "javascript:") {
		href = extractJSRedirect(href)
	}
	if href == "" {
		return ""
	}
	if base != nil {
		if u, err := neturl.Parse(href); err == nil {
			return base.ResolveReference(u).String()
		}
	}
	return href
}

func parseLinks(src string, base *neturl.URL) []string {
	links := make([]string, 0, 48)
	seen := make(map[string]struct{}, 48)

	add := func(href string) {
		href = resolveMaybeRelative(href, base)
		if href == "" {
			return
		}
		if _, ok := seen[href]; ok {
			return
		}
		seen[href] = struct{}{}
		links = append(links, href)
	}

	for _, mm := range linkRe.FindAllStringSubmatch(src, -1) {
		if len(mm) < 3 {
			continue
		}
		href := strings.TrimSpace(mm[1])
		if href == "" {
			continue
		}
		if isHiddenStyle(mm[0]) {
			continue
		}
		add(href)
	}

	for _, mm := range anchorTagRe.FindAllStringSubmatch(src, -1) {
		if len(mm) < 3 {
			continue
		}
		attrs := mm[1]
		if isHiddenStyle(attrs) {
			continue
		}
		href := extractAttr(attrs, "href")
		if href == "" {
			href = extractJSRedirect(extractAttr(attrs, "onclick"))
		}
		if href == "" {
			continue
		}
		add(href)
	}

	for _, mm := range buttonTagRe.FindAllStringSubmatch(src, -1) {
		if len(mm) < 3 {
			continue
		}
		attrs := mm[1]
		if isHiddenStyle(attrs) {
			continue
		}
		href := extractAttr(attrs, "formaction")
		if href == "" {
			href = extractJSRedirect(extractAttr(attrs, "onclick"))
		}
		if href == "" {
			href = extractAttr(attrs, "data-href")
		}
		if href == "" {
			continue
		}
		add(href)
	}

	return links
}

func parseForms(src string, base *neturl.URL) []Form {
	matches := formOpenRe.FindAllStringSubmatchIndex(src, -1)
	if len(matches) == 0 {
		return nil
	}
	forms := make([]Form, 0, len(matches))
	for _, m := range matches {
		if len(m) < 4 {
			continue
		}
		openTag := src[m[2]:m[3]]
		rest := src[m[1]:]
		end := formCloseRe.FindStringIndex(rest)
		body := ""
		if end != nil {
			body = rest[:end[0]]
		}
		form := Form{Method: strings.ToUpper(strings.TrimSpace(extractAttr(openTag, "method"))), Action: extractAttr(openTag, "action")}
		if form.Method == "" {
			form.Method = "GET"
		}
		if form.Action == "" && base != nil {
			form.Action = base.String()
		}
		if form.Action != "" && base != nil {
			if u, err := neturl.Parse(form.Action); err == nil {
				form.Action = base.ResolveReference(u).String()
			}
		}

		if form.Action == "" {
			if js := extractJSRedirect(extractAttr(openTag, "onsubmit")); js != "" {
				form.Action = resolveMaybeRelative(js, base)
			}
		}

		// Inputs
		for _, im := range inputRe.FindAllStringSubmatch(body, -1) {
			if len(im) < 2 {
				continue
			}
			attrs := im[1]
			if isHiddenStyle(attrs) {
				continue
			}
			typ := strings.ToLower(strings.TrimSpace(extractAttr(attrs, "type")))
			if typ == "" {
				typ = "text"
			}
			name := extractAttr(attrs, "name")
			val := extractAttr(attrs, "value")
			if val == "" {
				val = extractAttr(attrs, "placeholder")
			}
			if typ == "checkbox" && hasAttr(attrs, "checked") && val == "" {
				val = "on"
			}
			if typ == "radio" && hasAttr(attrs, "checked") && val == "" {
				val = "on"
			}
			if typ == "submit" || typ == "button" || typ == "image" {
				if name == "" {
					name = "_submit"
				}
				if val == "" {
					val = cleanInline(extractAttr(attrs, "alt"))
				}
				if val == "" {
					val = typ
				}
				if fa := extractAttr(attrs, "formaction"); fa != "" {
					form.Action = resolveMaybeRelative(fa, base)
				}
				if fm := strings.ToUpper(strings.TrimSpace(extractAttr(attrs, "formmethod"))); fm != "" {
					form.Method = fm
				}
				if js := extractJSRedirect(extractAttr(attrs, "onclick")); js != "" && form.Action == "" {
					form.Action = resolveMaybeRelative(js, base)
				}
				form.Fields = append(form.Fields, Field{Name: name, Type: typ, Value: val, Enabled: true})
				continue
			}
			if name == "" {
				continue
			}
			form.Fields = append(form.Fields, Field{Name: name, Type: typ, Value: val, Enabled: true})
		}

		// Buttons
		for _, bm := range buttonRe.FindAllStringSubmatch(body, -1) {
			if len(bm) < 3 {
				continue
			}
			attrs := bm[1]
			if isHiddenStyle(attrs) {
				continue
			}
			name := extractAttr(attrs, "name")
			if name == "" {
				continue
			}
			typ := strings.ToLower(strings.TrimSpace(extractAttr(attrs, "type")))
			if typ == "" {
				typ = "button"
			}
			val := extractAttr(attrs, "value")
			if val == "" {
				val = cleanInline(bm[2])
			}
			if val == "" {
				val = typ
			}
			if fa := extractAttr(attrs, "formaction"); fa != "" && form.Action == "" {
				form.Action = resolveMaybeRelative(fa, base)
			}
			if fm := strings.ToUpper(strings.TrimSpace(extractAttr(attrs, "formmethod"))); fm != "" {
				form.Method = fm
			}
			if js := extractJSRedirect(extractAttr(attrs, "onclick")); js != "" && form.Action == "" {
				form.Action = resolveMaybeRelative(js, base)
			}
			form.Fields = append(form.Fields, Field{Name: name, Type: typ, Value: val, Enabled: true})
		}

		// Textareas
		for _, tm := range textareaRe.FindAllStringSubmatch(body, -1) {
			if len(tm) < 3 {
				continue
			}
			attrs := tm[1]
			if isHiddenStyle(attrs) {
				continue
			}
			name := extractAttr(attrs, "name")
			if name == "" {
				continue
			}
			form.Fields = append(form.Fields, Field{Name: name, Type: "textarea", Value: cleanInline(tm[2])})
		}

		// Selects: pick selected option or first option
		for _, sm := range selectRe.FindAllStringSubmatch(body, -1) {
			if len(sm) < 3 {
				continue
			}
			attrs := sm[1]
			if isHiddenStyle(attrs) {
				continue
			}
			name := extractAttr(attrs, "name")
			if name == "" {
				continue
			}
			options := optionRe.FindAllStringSubmatch(sm[2], -1)
			picked := ""
			for _, om := range options {
				if len(om) < 3 {
					continue
				}
				optAttrs := om[1]
				if hasAttr(optAttrs, "selected") {
					picked = extractAttr(optAttrs, "value")
					if picked == "" {
						picked = cleanInline(om[2])
					}
					break
				}
				if picked == "" {
					picked = extractAttr(optAttrs, "value")
					if picked == "" {
						picked = cleanInline(om[2])
					}
				}
			}
			form.Fields = append(form.Fields, Field{Name: name, Type: "select", Value: picked})
		}

		if len(form.Fields) > 0 {
			forms = append(forms, form)
		}
	}
	return forms
}

func composeFormSummary(f Form) string {
	parts := make([]string, 0, len(f.Fields))
	for _, field := range f.Fields {
		if !field.Enabled {
			continue
		}
		parts = append(parts, field.Name+"="+field.Value)
	}
	sort.Strings(parts)
	if len(parts) > 3 {
		parts = parts[:3]
	}
	return fmt.Sprintf("%s %s [%s]", f.Method, f.Action, strings.Join(parts, ", "))
}

func renderText(base *neturl.URL, src string) (string, []string, []Form, string) {
	doc, baseURL := DOMAndCSSFromHTML(src, base, StyleContext{MediaType: "screen", ColorScheme: "light"})
	if doc == nil || doc.Root == nil {
		return "", nil, nil, ""
	}

	if redirect := ExecuteInlineScripts(doc, baseURL); redirect != "" {
		return "", nil, nil, redirect
	}

	textRoot := QuerySelector(doc.Root, "body")
	if textRoot == nil {
		textRoot = doc.Root
	}
	text := CollectText(textRoot)
	links := ExtractLinks(doc.Root, baseURL)
	forms := ExtractForms(doc.Root, baseURL)
	return text, links, forms, ""
}

func (b *browser) fetch(rawURL string) (page, error) {
	return b.fetchWithDepth(rawURL, 0)
}

func (b *browser) fetchWithDepth(rawURL string, depth int) (page, error) {
	normalized, parsed, err := normalizeURL(rawURL, nil)
	if err != nil {
		return page{}, err
	}

	req, err := http.NewRequest(http.MethodGet, normalized, nil)
	if err != nil {
		return page{}, err
	}
	req.Header.Set("User-Agent", "COS-TextBrowser/3.0")
	req.Header.Set("Accept", "text/html, text/plain;q=0.9, application/xhtml+xml;q=0.8, */*;q=0.1")
	req.Header.Set("Accept-Language", "en-US,en;q=0.8,ja;q=0.6")
	req.Header.Set("Cache-Control", "no-cache")

	resp, err := b.client.Do(req)
	if err != nil {
		return page{}, err
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return page{}, err
	}

	contentType := resp.Header.Get("Content-Type")
	mediaType, _, _ := mime.ParseMediaType(contentType)

	src := string(body)
	title := "(untitled)"
	text := ""
	links := []string{}
	forms := []Form{}

	switch {
	case strings.HasPrefix(mediaType, "text/plain"):
		if parsed != nil {
			title = parsed.Host
		}
		text = src
	default:
		if m := titleRe.FindStringSubmatch(src); len(m) > 1 {
			title = cleanInline(m[1])
		} else if parsed != nil && parsed.Host != "" {
			title = parsed.Host
		}
		base := resp.Request.URL
		if m := baseHrefRe.FindStringSubmatch(src); len(m) > 1 {
			if u, err := neturl.Parse(html.UnescapeString(strings.TrimSpace(m[1]))); err == nil {
				base = base.ResolveReference(u)
			}
		}
		redirect := ""
			text, links, forms, redirect = renderText(base, src)
			if redirect != "" && depth < 3 {
				return b.fetchWithDepth(redirect, depth+1)
			}
	}

	if title == "" {
		if parsed != nil {
			title = parsed.Host
		}
	}

	return page{
		URL:         resp.Request.URL.String(),
		Title:       title,
		Text:        text,
		Links:       links,
		Forms:       forms,
		ContentType: contentType,
		StatusCode:  resp.StatusCode,
		Fetched:     time.Now().UTC(),
	}, nil
}

func (b *browser) open(url string) error {
	p, err := b.fetch(url)
	if err != nil {
		return err
	}
	if b.pos < len(b.history)-1 {
		b.history = append([]page(nil), b.history[:b.pos+1]...)
	}
	b.history = append(b.history, p)
	b.pos = len(b.history) - 1
	return nil
}

func (b *browser) replaceCurrent(url string) error {
	p, err := b.fetch(url)
	if err != nil {
		return err
	}
	if b.pos < 0 || b.pos >= len(b.history) {
		b.history = []page{p}
		b.pos = 0
		return nil
	}
	b.history[b.pos] = p
	return nil
}

func (b *browser) current() *page {
	if b.pos < 0 || b.pos >= len(b.history) {
		return nil
	}
	return &b.history[b.pos]
}

func (b *browser) back() error {
	if b.pos <= 0 {
		return fmt.Errorf("history start")
	}
	b.pos--
	return nil
}

func (b *browser) forward() error {
	if b.pos >= len(b.history)-1 {
		return fmt.Errorf("history end")
	}
	b.pos++
	return nil
}

func buildURL(template, query string) string {
	return strings.ReplaceAll(template, "{q}", neturl.QueryEscape(strings.TrimSpace(query)))
}

func openSearch(br *browser, engine, query string) error {
	query = strings.TrimSpace(query)
	if query == "" {
		return fmt.Errorf("empty query")
	}
	switch strings.ToLower(strings.TrimSpace(engine)) {
	case "w", "wiki", "wikipedia":
		return br.open(buildURL("https://ja.wikipedia.org/wiki/Special:Search?search={q}", query))
	case "g", "google":
		return br.open(buildURL("https://www.google.com/search?gbv=1&hl=ja&q={q}", query))
	case "d", "duck", "ddg":
		return br.open(buildURL("https://html.duckduckgo.com/html/?q={q}", query))
	default:
		return fmt.Errorf("unknown search engine: %s", engine)
	}
}

func defaultSearchEngine() string {
	engine := strings.TrimSpace(os.Getenv("COS_BROWSER_DEFAULT_SEARCH"))
	if engine == "" {
		return "google"
	}
	return engine
}

func parseQuery(cmd string) (string, string) {
	cmd = strings.TrimSpace(cmd)
	if cmd == "" {
		return "", ""
	}
	parts := strings.Fields(cmd)
	if len(parts) == 0 {
		return "", ""
	}
	if len(parts) == 1 {
		return parts[0], ""
	}
	return parts[0], strings.TrimSpace(cmd[len(parts[0]):])
}

func submitForm(br *browser, p *page, idx int, assignments map[string]string) error {
	if idx < 1 || idx > len(p.Forms) {
		return fmt.Errorf("invalid form index")
	}
	f := p.Forms[idx-1]
	values := make(neturl.Values)
	for _, field := range f.Fields {
		if !field.Enabled {
			continue
		}
		values.Set(field.Name, field.Value)
	}
	for k, v := range assignments {
		values.Set(k, v)
	}

	method := strings.ToUpper(strings.TrimSpace(f.Method))
	if method == "" {
		method = "GET"
	}
	target := f.Action
	if target == "" {
		target = p.URL
	}
	if u, err := neturl.Parse(target); err == nil {
		if !u.IsAbs() {
			if resolved, err := resolveAgainst(target, p.URL); err == nil {
				target = resolved
			}
		} else {
			target = u.String()
		}
	}

	switch method {
	case "GET":
		sep := "?"
		if strings.Contains(target, "?") {
			sep = "&"
		}
		return br.open(target + sep + values.Encode())
	case "POST":
		body := strings.NewReader(values.Encode())
		req, err := http.NewRequest(http.MethodPost, target, body)
		if err != nil {
			return err
		}
		req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
		req.Header.Set("User-Agent", "COS-TextBrowser/3.0")
		req.Header.Set("Accept", "text/html, text/plain;q=0.9, application/xhtml+xml;q=0.8, */*;q=0.1")
		resp, err := br.client.Do(req)
		if err != nil {
			return err
		}
		defer resp.Body.Close()
		data, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
		if err != nil {
			return err
		}
		// Reuse fetch parsing logic by opening the response URL directly if redirected,
		// otherwise synthesize a page from the response body.
		finalURL := resp.Request.URL.String()
		if resp.StatusCode >= 300 && resp.StatusCode < 400 && finalURL != target {
			return br.open(finalURL)
		}
		mediaType, _, _ := mime.ParseMediaType(resp.Header.Get("Content-Type"))
		src := string(data)
		title := finalURL
		text := src
		links := []string{}
		forms := []Form{}
		if !strings.HasPrefix(mediaType, "text/plain") {
			if m := titleRe.FindStringSubmatch(src); len(m) > 1 {
				title = cleanInline(m[1])
			}
			if base, err := neturl.Parse(finalURL); err == nil {
				redirect := ""
				text, links, forms, redirect = renderText(base, src)
				if redirect != "" {
					return br.open(redirect)
				}
			}
		}
		p2 := page{URL: finalURL, Title: title, Text: text, Links: links, Forms: forms, ContentType: mediaType, StatusCode: resp.StatusCode, Fetched: time.Now().UTC()}
		if br.pos < len(br.history)-1 {
			br.history = append([]page(nil), br.history[:br.pos+1]...)
		}
		br.history = append(br.history, p2)
		br.pos = len(br.history) - 1
		return nil
	default:
		return fmt.Errorf("unsupported form method: %s", method)
	}
}

func findInPage(p *page, needle string) []string {
	needle = strings.ToLower(strings.TrimSpace(needle))
	if needle == "" || p == nil {
		return nil
	}
	lines := strings.Split(p.Text, "\n")
	out := make([]string, 0, 8)
	for i, ln := range lines {
		if strings.Contains(strings.ToLower(ln), needle) {
			out = append(out, fmt.Sprintf("%d: %s", i+1, ln))
			if len(out) >= 10 {
				break
			}
		}
	}
	return out
}

func printPage(p *page, showLinks, showForms bool) {
	status := ""
	if p.StatusCode != 0 {
		status = fmt.Sprintf(" [HTTP %d]", p.StatusCode)
	}
	fmt.Printf("\n== %s ==%s\n%s\n", p.Title, status, p.URL)
	if p.ContentType != "" {
		fmt.Printf("[%s] fetched %s\n", p.ContentType, p.Fetched.Format(time.RFC3339))
	}
	fmt.Println(strings.Repeat("-", 80))
	if p.Text != "" {
		fmt.Println(p.Text)
	} else {
		fmt.Println("(empty)")
	}
	if showLinks && len(p.Links) > 0 {
		fmt.Println(strings.Repeat("-", 80))
		for i, l := range p.Links {
			fmt.Printf("[%d] %s\n", i+1, l)
		}
	}
	if showForms && len(p.Forms) > 0 {
		fmt.Println(strings.Repeat("-", 80))
		for i, f := range p.Forms {
			fmt.Printf("{f%d} %s\n", i+1, composeFormSummary(f))
		}
	}
	fmt.Println(strings.Repeat("-", 80))
}

func main() {
	startURL := flag.String("url", "", "start url")
	homeURL := flag.String("home", "https://ja.wikipedia.org/wiki/メインページ", "home page url")
	showLinks := flag.Bool("links", true, "show extracted links")
	showForms := flag.Bool("forms", true, "show extracted forms")
	flag.Parse()

	br := newBrowser()
	if *startURL == "" {
		args := flag.Args()
		if len(args) > 0 {
			*startURL = strings.TrimSpace(strings.Join(args, " "))
		}
	}
	if *startURL == "" {
		*startURL = *homeURL
	}
	if err := br.open(*startURL); err != nil {
		fmt.Fprintln(os.Stderr, "open:", err)
		os.Exit(1)
	}

	in := bufio.NewReader(os.Stdin)
	for {
		cur := br.current()
		if cur == nil {
			fmt.Fprintln(os.Stderr, "no current page")
			os.Exit(1)
		}

		printPage(cur, *showLinks, *showForms)
		fmt.Print("[n <idx>] open link  [f <idx>] submit form  [u <url>] open url\n[ w <query>] wiki  [g <query>] google  [d <query>] duckduckgo\n[b] back  [fwd] forward  [r] reload  [q] quit\nTip: use `f <idx> name=value` to activate form buttons/controls.\n> ")

		line, _ := in.ReadString('\n')
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}

		cmd, rest := parseQuery(line)
		switch strings.ToLower(cmd) {
		case "q", "quit", ":q":
			return
		case "b", "back":
			if err := br.back(); err != nil {
				fmt.Fprintln(os.Stderr, err)
			}
		case "fwd", "forward":
			if err := br.forward(); err != nil {
				fmt.Fprintln(os.Stderr, err)
			}
		case "r", "reload":
			if err := br.replaceCurrent(cur.URL); err != nil {
				fmt.Fprintln(os.Stderr, err)
			}
		case "u", "open":
			if err := br.open(rest); err != nil {
				fmt.Fprintln(os.Stderr, err)
			}
		case "n", "link":
			idx, err := strconv.Atoi(strings.TrimSpace(rest))
			if err != nil || idx < 1 || idx > len(cur.Links) {
				fmt.Fprintln(os.Stderr, "invalid link index")
				continue
			}
			if err := br.open(cur.Links[idx-1]); err != nil {
				fmt.Fprintln(os.Stderr, err)
			}
		case "w", "wiki":
			if err := openSearch(br, "wiki", rest); err != nil {
				fmt.Fprintln(os.Stderr, err)
			}
		case "g", "google":
			if err := openSearch(br, "google", rest); err != nil {
				fmt.Fprintln(os.Stderr, err)
			}
		case "d", "duck", "ddg":
			if err := openSearch(br, "duck", rest); err != nil {
				fmt.Fprintln(os.Stderr, err)
			}
		case "/", "find":
			matches := findInPage(cur, rest)
			if len(matches) == 0 {
				fmt.Fprintln(os.Stderr, "no matches")
			} else {
				for _, m := range matches {
					fmt.Println(m)
				}
			}
		case "f", "form":
			parts := strings.Fields(rest)
			if len(parts) == 0 {
				fmt.Fprintln(os.Stderr, "usage: f <form-index> [name=value ...]")
				continue
			}
			idx, err := strconv.Atoi(parts[0])
			if err != nil {
				fmt.Fprintln(os.Stderr, "invalid form index")
				continue
			}
			assigns := map[string]string{}
			for _, pair := range parts[1:] {
				kv := strings.SplitN(pair, "=", 2)
				if len(kv) != 2 {
					continue
				}
				assigns[kv[0]] = kv[1]
			}
			if err := submitForm(br, cur, idx, assigns); err != nil {
				fmt.Fprintln(os.Stderr, err)
			}
		default:
			// Fallback: treat naked input as a search using the configured default engine.
			if err := openSearch(br, defaultSearchEngine(), line); err != nil {
				fmt.Fprintln(os.Stderr, err)
			}
		}
	}
}
