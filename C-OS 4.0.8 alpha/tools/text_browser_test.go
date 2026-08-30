package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	neturl "net/url"
	"strings"
	"testing"
)

func TestStripTagsHTMLFormatsContent(t *testing.T) {
	src := `<html><head><title>Ignored title</title><style>.x{}</style></head><body><h1>Hello</h1><p>Line one</p><ul><li>First</li><li>Second</li></ul></body></html>`
	got := stripTagsHTML(src)

	if strings.Contains(got, "Ignored title") {
		t.Fatalf("head content leaked into text: %q", got)
	}
	for _, want := range []string{"Hello", "Line one", "- First", "- Second"} {
		if !strings.Contains(got, want) {
			t.Fatalf("expected %q in output, got %q", want, got)
		}
	}
}

func TestRenderTextResolvesLinksAndForms(t *testing.T) {
	base, err := neturl.Parse("https://example.org/base/")
	if err != nil {
		t.Fatal(err)
	}
	src := `<html><head><base href="/base/"><title>Sample</title></head><body>
		<a href="page.html">Page</a>
		<form method="post" action="/submit">
			<input type="text" name="q" value="abc def">
			<input type="checkbox" name="agree" checked>
			<textarea name="msg">hello world</textarea>
			<select name="kind">
				<option value="x">X</option>
				<option selected value="y">Y</option>
			</select>
		</form>
	</body></html>`

	text, links, forms, redirect := renderText(base, src)
	if strings.Contains(text, "Sample") {
		t.Fatalf("title content should not appear in body text: %q", text)
	}
	if len(links) != 1 || links[0] != "https://example.org/base/page.html" {
		t.Fatalf("unexpected links: %#v", links)
	}
	if redirect != "" {
		t.Fatalf("unexpected redirect: %s", redirect)
	}
	if len(forms) != 1 {
		t.Fatalf("expected 1 form, got %#v", forms)
	}
	f := forms[0]
	if f.Method != "POST" || f.Action != "https://example.org/submit" {
		t.Fatalf("unexpected form metadata: %#v", f)
	}
	if len(f.Fields) != 4 {
		t.Fatalf("expected 4 form fields, got %#v", f.Fields)
	}
	got := map[string]string{}
	for _, field := range f.Fields {
		got[field.Name] = field.Value
	}
	if got["q"] != "abc def" || got["agree"] != "on" || got["msg"] != "hello world" || got["kind"] != "y" {
		t.Fatalf("unexpected field values: %#v", got)
	}
}

func TestBrowserNavigationAndSubmitForm(t *testing.T) {
	var lastPOST string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/":
			w.Header().Set("Content-Type", "text/html; charset=utf-8")
			_, _ = w.Write([]byte(`<html><head><title>Home</title><base href="/base/"></head><body><a href="page.html">Page</a><form method="post" action="/submit"><input name="q" value="abc"></form></body></html>`))
		case "/base/page.html":
			w.Header().Set("Content-Type", "text/html; charset=utf-8")
			_, _ = w.Write([]byte(`<html><head><title>Page</title></head><body>Page body <a href="../">Back</a></body></html>`))
		case "/submit":
			body, _ := io.ReadAll(r.Body)
			lastPOST = string(body)
			w.Header().Set("Content-Type", "text/html; charset=utf-8")
			_, _ = w.Write([]byte(`<html><head><title>Submitted</title></head><body>` + lastPOST + `</body></html>`))
		default:
			http.NotFound(w, r)
		}
	}))
	defer srv.Close()

	br := newBrowser()
	p, err := br.fetch(srv.URL + "/")
	if err != nil {
		t.Fatal(err)
	}
	if len(p.Links) != 1 || p.Links[0] != srv.URL+"/base/page.html" {
		t.Fatalf("unexpected fetched page links: %#v", p.Links)
	}
	br.history = []page{p}
	br.pos = 0

	if err := submitForm(br, &p, 1, map[string]string{"q": "xyz"}); err != nil {
		t.Fatal(err)
	}
	cur := br.current()
	if cur == nil {
		t.Fatal("current page is nil after submit")
	}
	if !strings.HasSuffix(cur.URL, "/submit") {
		t.Fatalf("unexpected submit URL: %s", cur.URL)
	}
	if !strings.Contains(cur.Text, "q=xyz") {
		t.Fatalf("submit body not rendered: %q", cur.Text)
	}
	if lastPOST != "q=xyz" {
		t.Fatalf("unexpected POST body: %q", lastPOST)
	}
}

func TestDOMParserAndQuerySelector(t *testing.T) {
	doc := ParseHTML(`<html><body><div class="box"><span id="x">Hello</span></div></body></html>`)
	if doc == nil || doc.Root == nil {
		t.Fatal("document not parsed")
	}
	span := QuerySelector(doc.Root, "div.box > span#x")
	if span == nil {
		t.Fatal("query selector failed")
	}
	if got := strings.TrimSpace(span.TextContent()); got != "Hello" {
		t.Fatalf("unexpected text content: %q", got)
	}
	if span.Parent == nil || span.Parent.Data != "div" {
		t.Fatalf("unexpected parent chain: %#v", span.Parent)
	}
}

func TestCSSParserAndCascade(t *testing.T) {
	doc := ParseHTML(`<html><head><style>.box { display: none; } div { display: block; } #target { visibility: hidden; }</style></head><body><div class="box" id="target"></div></body></html>`)
	if doc == nil {
		t.Fatal("document not parsed")
	}
	var css strings.Builder
	walkNodes(doc.Root, func(n *Node) {
		if n.Type == ElementNode && n.Data == "style" {
			css.WriteString(n.TextContent())
		}
	})
	sheet := ParseCSSStylesheet(css.String())
	ApplyStyles(doc, sheet, StyleContext{MediaType: "screen", ColorScheme: "light"})
	div := QuerySelector(doc.Root, "#target")
	if div == nil {
		t.Fatal("target div not found")
	}
	if got := strings.ToLower(strings.TrimSpace(div.ComputedValue("display"))); got != "none" {
		t.Fatalf("expected display none, got %q", got)
	}
	if !IsHidden(div) {
		t.Fatal("expected element to be hidden")
	}
}


func TestInlineScriptMutations(t *testing.T) {
	base, err := neturl.Parse("https://example.org/")
	if err != nil {
		t.Fatal(err)
	}
	src := `<html><head><title>Old</title><script>document.title = 'New';document.body.innerHTML = '<div id="x">Hello</div>';document.getElementById('x').textContent = 'World';</script></head><body>Ignored</body></html>`
	text, links, forms, redirect := renderText(base, src)
	if redirect != "" {
		t.Fatalf("unexpected redirect: %s", redirect)
	}
	if strings.Contains(text, "Ignored") {
		t.Fatalf("script mutation should replace body text, got %q", text)
	}
	if !strings.Contains(text, "World") {
		t.Fatalf("expected mutated text, got %q", text)
	}
	if len(links) != 0 || len(forms) != 0 {
		t.Fatalf("unexpected links/forms: %#v %#v", links, forms)
	}
}
