package org.vaadin.addons.enverhaase.vaadoom;

import com.vaadin.flow.component.html.H1;
import com.vaadin.flow.component.html.Paragraph;
import com.vaadin.flow.component.orderedlayout.VerticalLayout;
import com.vaadin.flow.router.Route;

/**
 * Local demo view for the Vaadoom add-on. Not shipped in the JAR (test scope).
 * Run with {@code mvn} and open http://localhost:8080/ to see the component.
 */
@Route("")
public class DemoView extends VerticalLayout {

    public DemoView() {
        setSizeFull();
        setAlignItems(Alignment.CENTER);

        add(new H1("Vaadoom demo"));
        add(new Paragraph(
                "The viewport below is the Vaadoom Flow component. In 0.1.0 it renders a "
                        + "placeholder test pattern; the WebAssembly SUBLEQ/DOOM engine is wired in later."));

        Vaadoom doom = new Vaadoom();
        add(doom);
    }
}
