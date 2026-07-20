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
                "The viewport below is the Vaadoom Flow component. It boots a NOMMU Linux on a "
                        + "SUBLEQ VM compiled to WebAssembly and runs the DOOM attract demo "
                        + "(first paint takes ~15 seconds)."));

        Vaadoom doom = new Vaadoom();
        add(doom);
    }
}
